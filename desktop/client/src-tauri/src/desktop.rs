use crate::config::{apply_network_overrides, build_node_config_with_base, default_config_string};
use crate::connection::ConnectionSnapshot;
use crate::launch_options::{append_launch_args, merge_launch_options};
use crate::lifecycle::{
    close_action, should_disconnect_on_exit, tray_primary_action, CloseAction, TrayPrimaryAction,
};
use crate::manual_nodes::{
    delete_manual_node, find_node, merge_nodes, node_source, upsert_manual_node, ManualNodeInput,
    NodeSource,
};
use crate::pinger::{probe_nodes, targets_for_nodes};
use crate::preferences::{
    load_preferences, save_preferences, update_setting, Preferences, SubscriptionSource,
};
use crate::process::{CommandSpec, ProcessEvent, ProcessManager};
use crate::subscription::{
    refresh_with, RefreshResult, SubscriptionDocument, MAX_SUBSCRIPTION_BYTES,
};
use serde::Serialize;
use serde_json::{json, Value};
use std::collections::BTreeMap;
use std::fs;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Mutex,
};
use std::time::Duration;
use tauri::menu::{Menu, MenuItem};
use tauri::tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent};
use tauri::{AppHandle, Emitter, Manager, State, WindowEvent, Wry};
use url::Url;

pub struct DesktopState {
    data_dir: PathBuf,
    preferences: Mutex<Preferences>,
    subscription: Mutex<Option<StoredSubscription>>,
    process: Mutex<ProcessManager>,
    last_node_id: Mutex<Option<String>>,
    tray_items: Mutex<Option<TrayItems>>,
    exit_requested: AtomicBool,
    proxy: Mutex<ProxySession>,
    _instance: crate::windows::InstanceGuard,
}

#[derive(Default)]
struct ProxySession {
    address: String,
    auto: bool,
    applied: bool,
}

struct TrayItems {
    status: MenuItem<Wry>,
    primary: MenuItem<Wry>,
}

struct StoredSubscription {
    document: SubscriptionDocument,
    fetched_at_ms: u64,
    cached: bool,
}

impl From<RefreshResult> for StoredSubscription {
    fn from(result: RefreshResult) -> Self {
        Self {
            document: result.document,
            fetched_at_ms: result.fetched_at_ms,
            cached: result.cached,
        }
    }
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct BootstrapPayload {
    subscription: Option<SubscriptionPayload>,
    config: String,
    launch_options: BTreeMap<String, Value>,
    settings: Value,
    connection: ConnectionSnapshot,
    current_node_id: Option<String>,
    network_overrides: Value,
    administrator: bool,
    proxy_recovery_pending: bool,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct SubscriptionPayload {
    url: String,
    name: String,
    updated_at: Option<String>,
    last_synced_at: u64,
    cached: bool,
    cache_age_minutes: u64,
    nodes: Vec<NodePayload>,
    sources: Vec<SubscriptionSource>,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct NodePayload {
    id: String,
    name: String,
    subtitle: String,
    address: String,
    latency_ms: Option<u32>,
    favorite: bool,
    source: NodeSource,
    config: Option<Value>,
    options: Option<Value>,
    source_id: String,
    source_name: String,
}

#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct ConnectPayload {
    pid: u32,
    network: Value,
    session_id: u64,
}

impl DesktopState {
    fn new(app: &AppHandle) -> Result<Self, String> {
        let mut data_dir = app
            .path()
            .app_data_dir()
            .map_err(|error| error.to_string())?;
        #[cfg(debug_assertions)]
        if let Some(directory) = std::env::var_os("OPENPPP2_CLIENT_DATA_DIR") {
            data_dir = PathBuf::from(directory);
        }
        fs::create_dir_all(&data_dir).map_err(|error| error.to_string())?;
        let ready = std::env::args()
            .find_map(|arg| arg.strip_prefix("--elevated-ready=").map(PathBuf::from));
        if let Some(path) = ready.as_ref() {
            if path.parent() != Some(data_dir.as_path())
                || !path
                    .file_name()
                    .is_some_and(|name| name.to_string_lossy().starts_with("elevated-"))
            {
                return Err("Invalid elevation handoff path".into());
            }
        }
        let name = format!("Local\\OpenPPP2Client-{}", data_dir.to_string_lossy());
        let name = name.replace(['/', ':'], "_").replace('\\', "_");
        let name = format!("Local\\{name}");
        let instance = crate::windows::instance(&name, ready.is_some())?;
        let mut preferences = load_preferences(&data_dir.join("preferences.json"))
            .map_err(|error| error.to_string())?;
        for source in preferences
            .subscriptions
            .iter_mut()
            .filter(|s| valid_source_id(&s.id))
        {
            if let Ok(result) = refresh_with(
                &source.url,
                &data_dir.join(format!("subscription-{}.json", source.id)),
                |_| Err("startup".into()),
            ) {
                source.last_synced_at = result.fetched_at_ms;
                source.cached = true;
            }
        }
        let subscription = load_sources(&data_dir, &preferences);
        let _ = crate::windows::restore_proxy(&data_dir.join("proxy-recovery.json"), false);
        let emitter = app.clone();
        let process = ProcessManager::new(move |event| {
            let exited = matches!(&event.event, ProcessEvent::Exited(_));
            let state = emitter.try_state::<DesktopState>();
            if let Some(state) = state.as_ref() {
                update_tray_phase(state, &event.connection.status);
                if let Ok(mut proxy) = state.proxy.lock() {
                    if exited
                        || event.connection.status == "reconnecting"
                        || event.connection.status == "error"
                    {
                        if proxy.applied {
                            if let Err(error) = crate::windows::restore_proxy(
                                &state.data_dir.join("proxy-recovery.json"),
                                false,
                            ) {
                                let _ = emitter.emit("client://tray-error", error);
                            }
                            proxy.applied = false;
                        }
                    } else if event.connection.status == "connected" && proxy.auto && !proxy.applied
                    {
                        if let Ok(address) = proxy.address.parse::<std::net::SocketAddr>() {
                            if std::net::TcpStream::connect_timeout(
                                &address,
                                Duration::from_millis(200),
                            )
                            .is_ok()
                            {
                                match crate::windows::apply_proxy(
                                    &state.data_dir.join("proxy-recovery.json"),
                                    &proxy.address,
                                ) {
                                    Ok(()) => proxy.applied = true,
                                    Err(error) => {
                                        proxy.auto = false;
                                        let _ = emitter.emit("client://tray-error", error);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            let _ = emitter.emit("client://process", event);
            if exited {
                if let Some(state) = state.as_ref() {
                    update_tray(state, false, None);
                }
            }
        });
        Ok(Self {
            data_dir,
            preferences: Mutex::new(preferences),
            subscription: Mutex::new(subscription),
            process: Mutex::new(process),
            last_node_id: Mutex::new(None),
            tray_items: Mutex::new(None),
            exit_requested: AtomicBool::new(false),
            proxy: Mutex::new(ProxySession::default()),
            _instance: instance,
        })
    }

    fn save_preferences(&self, preferences: &Preferences) -> Result<(), String> {
        save_preferences(&self.data_dir.join("preferences.json"), preferences)
            .map_err(|error| error.to_string())
    }
}

fn now_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64
}

fn valid_source_id(id: &str) -> bool {
    !id.is_empty() && id.len() <= 64 && id.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'-')
}

fn load_sources(directory: &Path, preferences: &Preferences) -> Option<StoredSubscription> {
    let mut nodes = Vec::new();
    let mut fetched_at_ms = 0;
    for source in preferences
        .subscriptions
        .iter()
        .filter(|s| s.enabled && valid_source_id(&s.id))
    {
        if let Ok(result) = refresh_with(
            &source.url,
            &directory.join(format!("subscription-{}.json", source.id)),
            |_| Err("startup".into()),
        ) {
            fetched_at_ms = fetched_at_ms.max(result.fetched_at_ms);
            nodes.extend(result.document.nodes.into_iter().map(|mut node| {
                node.id = format!("sub:{}:{}", source.id, node.id);
                node
            }));
        }
    }
    if nodes.is_empty() {
        return None;
    }
    Some(StoredSubscription {
        document: SubscriptionDocument {
            document_type: "openppp2-subscription".into(),
            version: 1,
            name: Some("Subscriptions".into()),
            profile_prefix: None,
            updated_at: None,
            nodes,
        },
        fetched_at_ms,
        cached: preferences
            .subscriptions
            .iter()
            .any(|s| s.enabled && s.cached),
    })
}

#[tauri::command]
fn client_save_subscription(
    mut source: SubscriptionSource,
    state: State<'_, DesktopState>,
) -> Result<SubscriptionPayload, String> {
    let parsed = Url::parse(source.url.trim()).map_err(|_| "Invalid subscription URL")?;
    if !["https", "http"].contains(&parsed.scheme()) || parsed.host_str().is_none() {
        return Err("Use an HTTP/HTTPS subscription URL".into());
    }
    if source.id.is_empty() {
        source.id = format!("s{}", now_ms());
    }
    if !valid_source_id(&source.id) || source.name.trim().is_empty() {
        return Err("Invalid subscription ID or name".into());
    }
    source.url = source.url.trim().into();
    if state
        .process
        .lock()
        .map_err(|_| "Process lock")?
        .is_running()
        && state
            .last_node_id
            .lock()
            .map_err(|_| "Node lock")?
            .as_ref()
            .is_some_and(|id| id.starts_with(&format!("sub:{}:", source.id)))
    {
        return Err("Disconnect before editing the active subscription".into());
    }
    let mut preferences = state.preferences.lock().map_err(|_| "Preferences lock")?;
    let mut candidate = preferences.clone();
    if let Some(index) = candidate
        .subscriptions
        .iter()
        .position(|s| s.id == source.id)
    {
        let previous = &candidate.subscriptions[index];
        if previous.url == source.url {
            source.last_synced_at = previous.last_synced_at;
            source.cached = previous.cached;
            source.error = previous.error.clone();
        } else {
            source.last_synced_at = 0;
            source.cached = false;
            source.error.clear();
        }
        candidate.subscriptions[index] = source;
    } else {
        source.last_synced_at = 0;
        source.cached = false;
        source.error.clear();
        candidate.subscriptions.push(source);
    }
    state.save_preferences(&candidate)?;
    let stored = load_sources(&state.data_dir, &candidate);
    let payload = subscription_payload(stored.as_ref(), &candidate);
    *state.subscription.lock().map_err(|_| "Subscription lock")? = stored;
    *preferences = candidate;
    Ok(payload)
}

#[tauri::command]
fn client_delete_subscription(
    source_id: String,
    state: State<'_, DesktopState>,
) -> Result<SubscriptionPayload, String> {
    if !valid_source_id(&source_id) {
        return Err("Invalid source ID".into());
    }
    let prefix = format!("sub:{source_id}:");
    if state
        .process
        .lock()
        .map_err(|_| "Process lock")?
        .is_running()
        && state
            .last_node_id
            .lock()
            .map_err(|_| "Node lock")?
            .as_ref()
            .is_some_and(|id| id.starts_with(&prefix))
    {
        return Err("Disconnect before deleting the active subscription".into());
    }
    let mut preferences = state.preferences.lock().map_err(|_| "Preferences lock")?;
    let mut candidate = preferences.clone();
    candidate.subscriptions.retain(|s| s.id != source_id);
    candidate.favorites.retain(|id| !id.starts_with(&prefix));
    state.save_preferences(&candidate)?;
    let stored = load_sources(&state.data_dir, &candidate);
    let payload = subscription_payload(stored.as_ref(), &candidate);
    *state.subscription.lock().map_err(|_| "Subscription lock")? = stored;
    *preferences = candidate;
    Ok(payload)
}

#[tauri::command]
fn client_update_network(
    overrides: Value,
    state: State<'_, DesktopState>,
) -> Result<Value, String> {
    crate::network::validate(&overrides)?;
    let mut preferences = state.preferences.lock().map_err(|_| "Preferences lock")?;
    let mut candidate = preferences.clone();
    candidate.network_overrides = overrides.clone();
    state.save_preferences(&candidate)?;
    *preferences = candidate;
    Ok(overrides)
}

#[tauri::command]
fn client_restore_proxy(force: bool, state: State<'_, DesktopState>) -> Result<(), String> {
    let mut proxy = state.proxy.lock().map_err(|_| "Proxy lock")?;
    proxy.auto = false;
    crate::windows::restore_proxy(&state.data_dir.join("proxy-recovery.json"), force)?;
    proxy.applied = false;
    Ok(())
}

#[tauri::command]
async fn client_pick_executable() -> Result<Option<String>, String> {
    tauri::async_runtime::spawn_blocking(crate::windows::pick_executable)
        .await
        .map_err(|e| e.to_string())?
}

#[tauri::command]
async fn client_elevate(app: AppHandle, state: State<'_, DesktopState>) -> Result<(), String> {
    if state
        .process
        .lock()
        .map_err(|_| "Process lock")?
        .is_running()
    {
        return Err("Disconnect before restarting".into());
    }
    let ready = state.data_dir.join(format!("elevated-{}.ready", now_ms()));
    crate::windows::elevate(&ready)?;
    tauri::async_runtime::spawn_blocking(move || {
        let deadline = std::time::Instant::now() + Duration::from_secs(30);
        while std::time::Instant::now() < deadline {
            if ready.exists() {
                let _ = fs::remove_file(&ready);
                return Ok(());
            }
            std::thread::sleep(Duration::from_millis(100));
        }
        Err("Elevated application did not become ready".to_string())
    })
    .await
    .map_err(|e| e.to_string())??;
    prepare_exit(&app);
    app.exit(0);
    Ok(())
}

#[tauri::command]
async fn client_kernel_info(state: State<'_, DesktopState>) -> Result<Value, String> {
    let configured = state
        .preferences
        .lock()
        .map_err(|_| "Preferences lock")?
        .ppp_path
        .clone();
    let path = resolve_ppp_path(&configured)?;
    tauri::async_runtime::spawn_blocking(move || crate::kernel::inspect(&path))
        .await
        .map_err(|e| e.to_string())?
}

#[tauri::command]
fn client_preview(
    node_id: Option<String>,
    state: State<'_, DesktopState>,
) -> Result<Value, String> {
    let preferences = state
        .preferences
        .lock()
        .map_err(|_| "Preferences lock")?
        .clone();
    let stored = state.subscription.lock().map_err(|_| "Subscription lock")?;
    let nodes = stored
        .as_ref()
        .map(|s| s.document.nodes.as_slice())
        .unwrap_or_default();
    let mut config = if let Some(node) = node_id
        .as_ref()
        .and_then(|id| find_node(&preferences.manual_nodes, nodes, id))
    {
        build_node_config_with_base(node, Some(&preferences.raw_config))
            .map_err(|e| e.to_string())?
    } else {
        serde_json::from_str(if preferences.raw_config.is_empty() {
            default_config_string()
        } else {
            &preferences.raw_config
        })
        .map_err(|e| e.to_string())?
    };
    if preferences.network_overrides.is_object() {
        apply_network_overrides(&mut config, &preferences.network_overrides)
            .map_err(|e| e.to_string())?;
    }
    let node_options = node_id
        .as_ref()
        .and_then(|id| find_node(&preferences.manual_nodes, nodes, id))
        .and_then(|n| n.options.as_ref());
    let options = merge_launch_options(&preferences.launch_options, node_options)
        .map_err(|e| e.to_string())?;
    let mut args = vec![format!("--mode={}", preferences.settings.connection_mode)];
    append_launch_args(&options, &mut args).map_err(|e| e.to_string())?;
    Ok(json!({ "config": crate::kernel::redact(config), "args": args }))
}

#[tauri::command]
fn client_open_data(state: State<'_, DesktopState>) -> Result<(), String> {
    std::process::Command::new("explorer.exe")
        .arg(&state.data_dir)
        .spawn()
        .map_err(|e| e.to_string())?;
    Ok(())
}

#[tauri::command]
fn client_bootstrap(state: State<'_, DesktopState>) -> Result<BootstrapPayload, String> {
    let preferences = state
        .preferences
        .lock()
        .map_err(|_| "设置状态锁已损坏".to_string())?
        .clone();
    let subscription = state
        .subscription
        .lock()
        .map_err(|_| "订阅状态锁已损坏".to_string())?
        .as_ref()
        .map(|stored| subscription_payload(Some(stored), &preferences));
    Ok(BootstrapPayload {
        subscription: subscription.or_else(|| Some(subscription_payload(None, &preferences))),
        config: if preferences.raw_config.is_empty() {
            default_config_string().into()
        } else {
            preferences.raw_config.clone()
        },
        launch_options: preferences.launch_options.clone(),
        settings: json!({
            "autostart": preferences.settings.autostart,
            "closeToTray": preferences.settings.close_to_tray,
            "disconnectOnExit": preferences.settings.disconnect_on_exit,
            "language": preferences.settings.language,
            "appearance": preferences.settings.appearance,
            "connectionMode": preferences.settings.connection_mode,
            "pppPath": preferences.ppp_path,
            "autoSystemProxy": preferences.settings.auto_system_proxy,
        }),
        connection: state.process.lock().map_err(|_| "Process lock")?.snapshot(),
        current_node_id: state.last_node_id.lock().map_err(|_| "Node lock")?.clone(),
        network_overrides: if preferences.network_overrides.is_object() {
            preferences.network_overrides.clone()
        } else {
            json!({})
        },
        administrator: crate::windows::administrator(),
        proxy_recovery_pending: state.data_dir.join("proxy-recovery.json").exists(),
    })
}

#[tauri::command]
async fn subscription_refresh(
    url: String,
    source_id: Option<String>,
    state: State<'_, DesktopState>,
) -> Result<SubscriptionPayload, String> {
    let parsed = Url::parse(&url).map_err(|_| "订阅地址无效".to_string())?;
    if parsed.scheme() != "https" && parsed.scheme() != "http" {
        return Err("订阅地址只支持 HTTP/HTTPS".into());
    }
    let source = {
        let mut preferences = state.preferences.lock().map_err(|_| "Preferences lock")?;
        let id = source_id.unwrap_or_else(|| format!("s{}", now_ms()));
        if !valid_source_id(&id) {
            return Err("Invalid source ID".into());
        }
        let source = preferences
            .subscriptions
            .iter()
            .find(|s| s.id == id)
            .cloned()
            .unwrap_or(SubscriptionSource {
                id,
                name: parsed.host_str().unwrap_or("Subscription").into(),
                url: url.clone(),
                enabled: true,
                last_synced_at: 0,
                cached: false,
                error: String::new(),
            });
        if !preferences.subscriptions.iter().any(|s| s.id == source.id) {
            preferences.subscriptions.push(source.clone());
            state.save_preferences(&preferences)?;
        }
        source
    };
    let cache_path = state
        .data_dir
        .join(format!("subscription-{}.json", source.id));
    let fetch_url = url.clone();
    let result = tauri::async_runtime::spawn_blocking(move || {
        let client = reqwest::blocking::Client::builder()
            .timeout(Duration::from_secs(15))
            .user_agent("OpenPPP2/Desktop")
            .build()
            .map_err(|error| error.to_string())?;
        refresh_with(&fetch_url, &cache_path, |target| {
            let mut response = client
                .get(target)
                .header("Accept", "application/json")
                .send()
                .and_then(reqwest::blocking::Response::error_for_status)
                .map_err(|error| error.to_string())?;
            let mut body = Vec::new();
            response
                .by_ref()
                .take((MAX_SUBSCRIPTION_BYTES + 1) as u64)
                .read_to_end(&mut body)
                .map_err(|error| error.to_string())?;
            Ok(body)
        })
        .map_err(|error| error.to_string())
    })
    .await
    .map_err(|error| error.to_string())?;
    let mut preferences = state
        .preferences
        .lock()
        .map_err(|_| "设置状态锁已损坏".to_string())?;
    let item = preferences
        .subscriptions
        .iter_mut()
        .find(|s| s.id == source.id)
        .ok_or("Subscription removed during refresh")?;
    if item.url != url {
        return Err("Subscription URL changed during refresh".into());
    }
    match result {
        Ok(result) => {
            item.last_synced_at = result.fetched_at_ms;
            item.cached = result.cached;
            item.error = if result.cached {
                "Refresh failed; using cache".into()
            } else {
                String::new()
            };
        }
        Err(error) => {
            item.error = error;
            item.cached = true;
        }
    }
    state.save_preferences(&preferences)?;
    let stored = load_sources(&state.data_dir, &preferences);
    let payload = subscription_payload(stored.as_ref(), &preferences);
    *state
        .subscription
        .lock()
        .map_err(|_| "订阅状态锁已损坏".to_string())? = stored;
    Ok(payload)
}

#[tauri::command]
async fn client_probe_latency(
    node_ids: Option<Vec<String>>,
    state: State<'_, DesktopState>,
    app: AppHandle,
) -> Result<BTreeMap<String, Option<u32>>, String> {
    let preferences = state
        .preferences
        .lock()
        .map_err(|_| "设置状态锁已损坏".to_string())?
        .clone();
    let subscription_nodes = state
        .subscription
        .lock()
        .map_err(|_| "订阅状态锁已损坏".to_string())?
        .as_ref()
        .map(|stored| stored.document.nodes.clone())
        .unwrap_or_default();
    let nodes = merge_nodes(&preferences.manual_nodes, &subscription_nodes);
    let targets = targets_for_nodes(&nodes, node_ids.as_deref());
    let latencies = tauri::async_runtime::spawn_blocking(move || {
        probe_nodes(targets, 4, Duration::from_secs(3)).map_err(|error| error.to_string())
    })
    .await
    .map_err(|error| error.to_string())??;
    let _ = app.emit("client://latency", &latencies);
    Ok(latencies)
}

#[tauri::command]
fn client_connect(
    node_id: String,
    state: State<'_, DesktopState>,
) -> Result<ConnectPayload, String> {
    connect_node(&node_id, &state)
}

fn connect_node(node_id: &str, state: &DesktopState) -> Result<ConnectPayload, String> {
    let preferences = state
        .preferences
        .lock()
        .map_err(|_| "设置状态锁已损坏".to_string())?
        .clone();
    let subscription_nodes = state
        .subscription
        .lock()
        .map_err(|_| "订阅状态锁已损坏".to_string())?
        .as_ref()
        .map(|stored| stored.document.nodes.clone())
        .unwrap_or_default();
    let node = find_node(&preferences.manual_nodes, &subscription_nodes, node_id)
        .cloned()
        .ok_or_else(|| "找不到所选节点".to_string())?;
    let mut config = build_node_config_with_base(&node, Some(&preferences.raw_config))
        .map_err(|error| error.to_string())?;
    if preferences.network_overrides.is_object() {
        apply_network_overrides(&mut config, &preferences.network_overrides)
            .map_err(|e| e.to_string())?;
    }
    if preferences.settings.connection_mode == "client" && !crate::windows::administrator() {
        return Err("Virtual adapter requires administrator privileges; restart as administrator in Settings".into());
    }
    let mut process = state.process.lock().map_err(|_| "Process lock")?;
    if process.is_running() {
        return Err("Disconnect before connecting another node".into());
    }
    let runtime_dir = state.data_dir.join("runtime");
    fs::create_dir_all(&runtime_dir).map_err(|error| error.to_string())?;
    let config_path = runtime_dir.join("appsettings.json");
    write_atomic(
        &config_path,
        &serde_json::to_vec_pretty(&config).map_err(|error| error.to_string())?,
    )?;
    let stats_path = runtime_dir.join(format!("stats-{}.ndjson", now_ms()));
    let mode = if preferences.settings.connection_mode == "proxy" {
        "proxy"
    } else {
        "client"
    };
    let executable = resolve_ppp_path(&preferences.ppp_path)?;
    let mut spec = CommandSpec::new(
        executable,
        [
            format!("--mode={mode}"),
            format!("--config={}", config_path.display()),
            format!("--stats-json={}", stats_path.display()),
        ],
    );
    let launch_options = merge_launch_options(&preferences.launch_options, node.options.as_ref())
        .map_err(|error| error.to_string())?;
    append_launch_args(&launch_options, &mut spec.args).map_err(|error| error.to_string())?;
    let network = network_payload(&launch_options, &config);
    let address = network["httpProxy"].as_str().unwrap_or_default().to_owned();
    if mode == "proxy" && preferences.settings.auto_system_proxy {
        let endpoint: std::net::SocketAddr = address
            .parse()
            .map_err(|_| "Configure a loopback HTTP proxy port before connecting")?;
        if !endpoint.ip().is_loopback() || endpoint.port() == 0 {
            return Err("System proxy requires a loopback HTTP listener".into());
        }
        let listener = std::net::TcpListener::bind(endpoint)
            .map_err(|e| format!("HTTP proxy port unavailable: {e}"))?;
        drop(listener);
    }
    *state.proxy.lock().map_err(|_| "Proxy lock")? = ProxySession {
        address,
        auto: mode == "proxy" && preferences.settings.auto_system_proxy,
        applied: false,
    };
    spec.stats_path = Some(stats_path);
    let pid = process.start(spec).map_err(|error| error.to_string())?;
    *state
        .last_node_id
        .lock()
        .map_err(|_| "最近节点状态锁已损坏".to_string())? = Some(node.id.clone());
    update_tray_phase(state, "starting");
    Ok(ConnectPayload {
        pid,
        network,
        session_id: process.snapshot().session_id,
    })
}

#[tauri::command]
fn client_disconnect(state: State<'_, DesktopState>) -> Result<(), String> {
    state
        .process
        .lock()
        .map_err(|_| "进程状态锁已损坏".to_string())?
        .stop()
        .map_err(|error| error.to_string())?;
    crate::windows::restore_proxy(&state.data_dir.join("proxy-recovery.json"), false)
}

#[tauri::command]
fn client_update_config(config: String, state: State<'_, DesktopState>) -> Result<(), String> {
    let value: Value = serde_json::from_str(&config).map_err(|error| error.to_string())?;
    if !value.is_object() {
        return Err("配置根节点必须是 JSON object".into());
    }
    let mut preferences = state
        .preferences
        .lock()
        .map_err(|_| "设置状态锁已损坏".to_string())?;
    let mut candidate = preferences.clone();
    candidate.raw_config =
        serde_json::to_string_pretty(&value).map_err(|error| error.to_string())?;
    state.save_preferences(&candidate)?;
    *preferences = candidate;
    Ok(())
}

fn validated_client_config(
    config: &str,
    options: Value,
) -> Result<(String, BTreeMap<String, Value>), String> {
    let config_value: Value = serde_json::from_str(config).map_err(|error| error.to_string())?;
    if !config_value.is_object() {
        return Err("配置根节点必须是 JSON object".into());
    }
    let object = options
        .as_object()
        .ok_or_else(|| "启动参数必须是 JSON object".to_string())?;
    let launch_options: BTreeMap<String, Value> = object
        .iter()
        .map(|(key, value)| (key.clone(), value.clone()))
        .collect();
    append_launch_args(&launch_options, &mut Vec::new()).map_err(|error| error.to_string())?;
    let raw_config =
        serde_json::to_string_pretty(&config_value).map_err(|error| error.to_string())?;
    Ok((raw_config, launch_options))
}

#[tauri::command]
fn client_update_client_config(
    config: String,
    options: Value,
    state: State<'_, DesktopState>,
) -> Result<BTreeMap<String, Value>, String> {
    let (raw_config, launch_options) = validated_client_config(&config, options)?;
    let mut preferences = state
        .preferences
        .lock()
        .map_err(|_| "设置状态锁已损坏".to_string())?;
    let mut candidate = preferences.clone();
    candidate.raw_config = raw_config;
    candidate.launch_options = launch_options.clone();
    state.save_preferences(&candidate)?;
    *preferences = candidate;
    Ok(launch_options)
}

#[tauri::command]
fn client_upsert_manual_node(
    node: ManualNodeInput,
    state: State<'_, DesktopState>,
) -> Result<SubscriptionPayload, String> {
    let mut preferences = state
        .preferences
        .lock()
        .map_err(|_| "设置状态锁已损坏".to_string())?;
    let mut candidate = preferences.clone();
    upsert_manual_node(&mut candidate.manual_nodes, node).map_err(|error| error.to_string())?;
    state.save_preferences(&candidate)?;
    *preferences = candidate;
    let snapshot = preferences.clone();
    drop(preferences);
    current_subscription_payload(&state, &snapshot)
}

#[tauri::command]
fn client_delete_manual_node(
    node_id: String,
    state: State<'_, DesktopState>,
) -> Result<SubscriptionPayload, String> {
    let running = state
        .process
        .lock()
        .map_err(|_| "进程状态锁已损坏".to_string())?
        .is_running();
    let is_current = state
        .last_node_id
        .lock()
        .map_err(|_| "最近节点状态锁已损坏".to_string())?
        .as_deref()
        == Some(&node_id);
    if running && is_current {
        return Err("正在使用的节点不能删除，请先断开连接".into());
    }

    let mut preferences = state
        .preferences
        .lock()
        .map_err(|_| "设置状态锁已损坏".to_string())?;
    let mut candidate = preferences.clone();
    delete_manual_node(&mut candidate.manual_nodes, &node_id).map_err(|error| error.to_string())?;
    candidate.favorites.remove(&node_id);
    state.save_preferences(&candidate)?;
    *preferences = candidate;
    let snapshot = preferences.clone();
    drop(preferences);
    if is_current {
        *state
            .last_node_id
            .lock()
            .map_err(|_| "最近节点状态锁已损坏".to_string())? = None;
    }
    current_subscription_payload(&state, &snapshot)
}

#[tauri::command]
fn client_update_launch_options(
    options: Value,
    state: State<'_, DesktopState>,
) -> Result<BTreeMap<String, Value>, String> {
    let object = options
        .as_object()
        .ok_or_else(|| "启动参数必须是 JSON object".to_string())?;
    let launch_options: BTreeMap<String, Value> = object
        .iter()
        .map(|(key, value)| (key.clone(), value.clone()))
        .collect();
    append_launch_args(&launch_options, &mut Vec::new()).map_err(|error| error.to_string())?;
    let mut preferences = state
        .preferences
        .lock()
        .map_err(|_| "设置状态锁已损坏".to_string())?;
    let mut candidate = preferences.clone();
    candidate.launch_options = launch_options.clone();
    state.save_preferences(&candidate)?;
    *preferences = candidate;
    Ok(launch_options)
}

#[tauri::command]
fn client_update_setting(
    key: String,
    value: Value,
    state: State<'_, DesktopState>,
) -> Result<(), String> {
    let mut preferences = state
        .preferences
        .lock()
        .map_err(|_| "设置状态锁已损坏".to_string())?;
    let mut candidate = preferences.clone();
    update_setting(&mut candidate, &key, value).map_err(|error| error.to_string())?;
    if key == "autostart" {
        crate::windows::autostart(candidate.settings.autostart)?;
    }
    if key == "connectionMode"
        && state
            .process
            .lock()
            .map_err(|_| "Process lock")?
            .is_running()
    {
        return Err("Disconnect before changing connection mode".into());
    }
    if key == "autoSystemProxy"
        && state
            .process
            .lock()
            .map_err(|_| "Process lock")?
            .is_running()
    {
        return Err("Disconnect before changing system proxy policy".into());
    }
    if let Err(error) = state.save_preferences(&candidate) {
        if key == "autostart" {
            let _ = crate::windows::autostart(preferences.settings.autostart);
        }
        return Err(error);
    }
    *preferences = candidate;
    Ok(())
}

#[tauri::command]
fn client_toggle_favorite(node_id: String, state: State<'_, DesktopState>) -> Result<(), String> {
    let mut preferences = state
        .preferences
        .lock()
        .map_err(|_| "设置状态锁已损坏".to_string())?;
    let mut candidate = preferences.clone();
    if !candidate.favorites.remove(&node_id) {
        candidate.favorites.insert(node_id);
    }
    state.save_preferences(&candidate)?;
    *preferences = candidate;
    Ok(())
}

fn current_subscription_payload(
    state: &DesktopState,
    preferences: &Preferences,
) -> Result<SubscriptionPayload, String> {
    let subscription = state
        .subscription
        .lock()
        .map_err(|_| "订阅状态锁已损坏".to_string())?;
    Ok(subscription_payload(subscription.as_ref(), preferences))
}

fn subscription_payload(
    stored: Option<&StoredSubscription>,
    preferences: &Preferences,
) -> SubscriptionPayload {
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis() as u64;
    let subscription_nodes = stored
        .map(|stored| stored.document.nodes.as_slice())
        .unwrap_or_default();
    let nodes = merge_nodes(&preferences.manual_nodes, subscription_nodes);
    let fetched_at_ms = stored.map(|stored| stored.fetched_at_ms).unwrap_or(0);
    SubscriptionPayload {
        url: preferences.subscription_url.clone(),
        name: stored
            .and_then(|stored| stored.document.name.clone())
            .clone()
            .unwrap_or_else(|| "本地节点".into()),
        updated_at: stored.and_then(|stored| stored.document.updated_at.clone()),
        last_synced_at: fetched_at_ms,
        cached: stored.is_some_and(|stored| stored.cached),
        cache_age_minutes: if fetched_at_ms == 0 {
            0
        } else {
            now.saturating_sub(fetched_at_ms) / 60_000
        },
        nodes: nodes
            .iter()
            .map(|node| NodePayload {
                id: node.id.clone(),
                name: node.name.clone(),
                subtitle: node.subtitle.clone(),
                address: display_address(node.server.as_deref().unwrap_or_default()),
                latency_ms: None,
                favorite: preferences.favorites.contains(&node.id),
                source: node_source(&preferences.manual_nodes, &node.id),
                config: node.config.clone().or_else(|| {
                    build_node_config_with_base(node, Some(&preferences.raw_config)).ok()
                }),
                options: node.options.clone(),
                source_id: node
                    .id
                    .strip_prefix("sub:")
                    .and_then(|id| id.split(':').next())
                    .unwrap_or("manual")
                    .into(),
                source_name: preferences
                    .subscriptions
                    .iter()
                    .find(|s| node.id.starts_with(&format!("sub:{}:", s.id)))
                    .map(|s| s.name.clone())
                    .unwrap_or_else(|| "Local".into()),
            })
            .collect(),
        sources: preferences.subscriptions.clone(),
    }
}

fn display_address(server: &str) -> String {
    let body = server.strip_prefix("ppp://").unwrap_or(server);
    let body = body
        .strip_prefix("ws/")
        .or_else(|| body.strip_prefix("wss/"))
        .unwrap_or(body);
    body.split('/').next().unwrap_or(body).to_owned()
}

fn network_payload(options: &BTreeMap<String, Value>, config: &Value) -> Value {
    let client = config.get("client");
    let proxy = |name: &str| {
        client
            .and_then(|value| value.get(name))
            .and_then(Value::as_object)
            .map(|value| {
                format!(
                    "{}:{}",
                    value
                        .get("bind")
                        .and_then(Value::as_str)
                        .unwrap_or("127.0.0.1"),
                    value.get("port").and_then(Value::as_u64).unwrap_or(0)
                )
            })
            .filter(|value| !value.ends_with(":0"))
            .unwrap_or_default()
    };
    json!({
        "tunIp": options.get("tunIp").and_then(Value::as_str).unwrap_or(""),
        "gateway": options.get("gateway").and_then(Value::as_str).unwrap_or(""),
        "httpProxy": proxy("http-proxy"), "socksProxy": proxy("socks-proxy"),
    })
}

fn resolve_ppp_path(configured: &str) -> Result<PathBuf, String> {
    let path = if configured.trim().is_empty() {
        let name = if cfg!(windows) { "ppp.exe" } else { "ppp" };
        std::env::current_exe()
            .map_err(|error| error.to_string())?
            .parent()
            .unwrap_or(Path::new("."))
            .join(name)
    } else {
        PathBuf::from(configured)
    };
    if !path.is_file() {
        return Err(format!("找不到 ppp 可执行文件: {}", path.display()));
    }
    Ok(path)
}

fn write_atomic(path: &Path, bytes: &[u8]) -> Result<(), String> {
    crate::storage::write(path, bytes)
}

fn update_tray(state: &DesktopState, running: bool, node_name: Option<&str>) {
    let Ok(items) = state.tray_items.lock() else {
        return;
    };
    let Some(items) = items.as_ref() else {
        return;
    };
    let status = if running {
        match node_name {
            Some(name) => format!("状态：已连接 - {name}"),
            None => "状态：已连接".to_string(),
        }
    } else {
        "状态：未连接".to_string()
    };
    let _ = items.status.set_text(status);
    let _ = items
        .primary
        .set_text(if running { "断开连接" } else { "连接" });
}

fn update_tray_phase(state: &DesktopState, phase: &str) {
    update_tray(state, !["disconnected", "error"].contains(&phase), None);
    if let Ok(items) = state.tray_items.lock() {
        if let Some(items) = items.as_ref() {
            let _ = items.status.set_text(format!("OpenPPP2: {phase}"));
        }
    }
}

fn show_main_window(app: &AppHandle) {
    if let Some(window) = app.get_webview_window("main") {
        let _ = window.show();
        let _ = window.unminimize();
        let _ = window.set_focus();
    }
}

fn handle_tray_primary(app: &AppHandle) -> Result<(), String> {
    let state = app.state::<DesktopState>();
    let running = state
        .process
        .lock()
        .map_err(|_| "进程状态锁已损坏".to_string())?
        .is_running();
    let last_node_id = state
        .last_node_id
        .lock()
        .map_err(|_| "最近节点状态锁已损坏".to_string())?
        .clone();
    match tray_primary_action(running, last_node_id.as_deref()) {
        TrayPrimaryAction::Connect(node_id) => {
            connect_node(&node_id, &state)?;
        }
        TrayPrimaryAction::Disconnect => {
            state
                .process
                .lock()
                .map_err(|_| "进程状态锁已损坏".to_string())?
                .stop()
                .map_err(|error| error.to_string())?;
            update_tray(&state, false, None);
        }
        TrayPrimaryAction::ShowWindow => show_main_window(app),
    }
    Ok(())
}

fn prepare_exit(app: &AppHandle) {
    let state = app.state::<DesktopState>();
    if state.exit_requested.swap(true, Ordering::SeqCst) {
        return;
    }
    if should_disconnect_on_exit(true) {
        let result = state
            .process
            .lock()
            .map_err(|_| "进程状态锁已损坏".to_string())
            .and_then(|mut process| process.stop().map_err(|error| error.to_string()));
        if let Err(error) = result {
            let _ = app.emit("client://tray-error", error);
        }
        if let Err(error) =
            crate::windows::restore_proxy(&state.data_dir.join("proxy-recovery.json"), false)
        {
            let _ = app.emit("client://tray-error", error);
        }
    }
}

fn setup_tray(app: &tauri::App) -> tauri::Result<()> {
    let status = MenuItem::with_id(app, "status", "状态：未连接", false, None::<&str>)?;
    let primary = MenuItem::with_id(app, "primary", "连接", true, None::<&str>)?;
    let show = MenuItem::with_id(app, "show", "打开 OpenPPP2", true, None::<&str>)?;
    let exit = MenuItem::with_id(app, "exit", "退出", true, None::<&str>)?;
    let menu = Menu::with_items(app, &[&status, &primary, &show, &exit])?;

    let mut builder = TrayIconBuilder::with_id("main")
        .menu(&menu)
        .show_menu_on_left_click(false)
        .tooltip("OpenPPP2 Client")
        .on_menu_event(|app, event| match event.id.as_ref() {
            "primary" => {
                if let Err(error) = handle_tray_primary(app) {
                    let _ = app.emit("client://tray-error", error);
                    show_main_window(app);
                }
            }
            "show" => show_main_window(app),
            "exit" => {
                prepare_exit(app);
                app.exit(0);
            }
            _ => {}
        })
        .on_tray_icon_event(|tray, event| {
            if let TrayIconEvent::Click {
                button: MouseButton::Left,
                button_state: MouseButtonState::Up,
                ..
            } = event
            {
                show_main_window(tray.app_handle());
            }
        });
    if let Some(icon) = app.default_window_icon() {
        builder = builder.icon(icon.clone());
    }
    builder.build(app)?;

    if let Ok(mut items) = app.state::<DesktopState>().tray_items.lock() {
        *items = Some(TrayItems { status, primary });
    }
    Ok(())
}

pub fn run() {
    if !cfg!(windows) {
        eprintln!("OpenPPP2 Client 只支持 Windows，并需要外部 ppp.exe。");
        return;
    }
    let app = tauri::Builder::default()
        .setup(|app| {
            app.manage(DesktopState::new(&app.handle())?);
            setup_tray(app)?;
            if let Some(path) = std::env::args()
                .find_map(|arg| arg.strip_prefix("--elevated-ready=").map(PathBuf::from))
            {
                fs::write(path, b"ready")?;
            }
            Ok(())
        })
        .on_window_event(|window, event| {
            if window.label() != "main" {
                return;
            }
            if let WindowEvent::CloseRequested { api, .. } = event {
                let state = window.state::<DesktopState>();
                let close_to_tray = state
                    .preferences
                    .lock()
                    .map(|preferences| preferences.settings.close_to_tray)
                    .unwrap_or(true);
                match close_action(close_to_tray, state.exit_requested.load(Ordering::SeqCst)) {
                    CloseAction::HideToTray => {
                        api.prevent_close();
                        let _ = window.hide();
                    }
                    CloseAction::Exit => {
                        api.prevent_close();
                        prepare_exit(window.app_handle());
                        window.app_handle().exit(0);
                    }
                }
            }
        })
        .invoke_handler(tauri::generate_handler![
            client_bootstrap,
            subscription_refresh,
            client_probe_latency,
            client_connect,
            client_disconnect,
            client_update_config,
            client_update_client_config,
            client_upsert_manual_node,
            client_delete_manual_node,
            client_update_launch_options,
            client_update_setting,
            client_toggle_favorite,
            client_save_subscription,
            client_delete_subscription,
            client_update_network,
            client_restore_proxy,
            client_pick_executable,
            client_elevate,
            client_kernel_info,
            client_preview,
            client_open_data,
        ])
        .build(tauri::generate_context!())
        .expect("failed to build OpenPPP2 Client");
    app.run(|app, event| {
        if matches!(event, tauri::RunEvent::ExitRequested { .. }) {
            prepare_exit(app);
        }
    });
}

#[cfg(test)]
mod tests {
    use super::{load_sources, subscription_payload, validated_client_config, StoredSubscription};
    use crate::manual_nodes::upsert_manual_node;
    use crate::manual_nodes::{ManualNodeInput, NodeSource};
    use crate::preferences::Preferences;
    use crate::subscription::{parse_subscription, RefreshResult};
    use serde_json::json;

    #[test]
    fn stored_subscription_keeps_cache_metadata() {
        let document = parse_subscription(br#"{"type":"openppp2-subscription","version":1,"nodes":[{"id":"n","name":"N","server":"ppp://127.0.0.1:1/","key":{"protocol-key":"p"}}]}"#).unwrap();
        let stored = StoredSubscription::from(RefreshResult {
            document,
            cached: true,
            fetched_at_ms: 42,
        });
        assert!(stored.cached);
        assert_eq!(stored.fetched_at_ms, 42);
    }

    #[test]
    fn combined_payload_marks_manual_nodes_and_exposes_edit_data() {
        let mut preferences = Preferences::default();
        upsert_manual_node(
            &mut preferences.manual_nodes,
            ManualNodeInput {
                id: None,
                name: "Local".into(),
                subtitle: "Desktop".into(),
                config: json!({
                    "key": { "protocol-key": "p" },
                    "client": { "server": "ppp://127.0.0.1:20000/" }
                }),
                options: json!({ "mux": 2 }),
            },
        )
        .unwrap();
        let payload = subscription_payload(None, &preferences);
        assert_eq!(payload.nodes.len(), 1);
        assert_eq!(payload.nodes[0].source, NodeSource::Manual);
        assert!(payload.nodes[0]
            .config
            .as_ref()
            .is_some_and(|value| value.is_object()));
        assert_eq!(payload.nodes[0].options.as_ref().unwrap()["mux"], 2);
    }

    #[test]
    fn combined_client_config_validates_both_inputs_before_save() {
        let (config, options) = validated_client_config(
            r#"{"concurrent":2}"#,
            json!({ "mux": 4, "muxMode": "flow", "vnet": true }),
        )
        .unwrap();
        assert!(config.contains("\"concurrent\": 2"));
        assert_eq!(options["mux"], 4);

        assert!(validated_client_config("[]", json!({})).is_err());
        assert!(validated_client_config(
            r#"{"concurrent":2}"#,
            json!({ "muxMode": "unsupported" }),
        )
        .is_err());
    }

    #[test]
    fn separate_subscriptions_keep_duplicate_ids_and_isolate_caches() {
        use crate::preferences::SubscriptionSource;
        use crate::subscription::refresh_with;
        let directory = tempfile::tempdir().unwrap();
        let mut preferences = Preferences::default();
        for id in ["one", "two"] {
            let url = format!("https://{id}.test/sub");
            let source = SubscriptionSource {
                id: id.into(),
                name: id.into(),
                url: url.clone(),
                enabled: true,
                last_synced_at: 0,
                cached: false,
                error: String::new(),
            };
            refresh_with(&url,&directory.path().join(format!("subscription-{id}.json")),|_| Ok(br#"{"type":"openppp2-subscription","version":1,"nodes":[{"id":"same","config":{}}]}"#.to_vec())).unwrap();
            preferences.subscriptions.push(source);
        }
        let stored = load_sources(directory.path(), &preferences).unwrap();
        assert_eq!(stored.document.nodes[0].id, "sub:one:same");
        assert_eq!(stored.document.nodes[1].id, "sub:two:same");
        preferences.subscriptions[0].url = "https://changed.test/sub".into();
        let stored = load_sources(directory.path(), &preferences).unwrap();
        assert_eq!(stored.document.nodes.len(), 1);
        assert_eq!(stored.document.nodes[0].id, "sub:two:same");
    }
}
