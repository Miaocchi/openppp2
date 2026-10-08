use crate::connection::ConnectionSnapshot;
use crate::stats::{StatsSampler, StatsView};
use crate::telemetry::{classify_line, TelemetryEvent};
use serde::Serialize;
use std::io::{BufRead, BufReader, Read, Seek, SeekFrom};
use std::path::PathBuf;
use std::process::{Child, Command, Stdio};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};
use thiserror::Error;

#[derive(Clone, Debug)]
pub struct CommandSpec {
    pub program: PathBuf,
    pub args: Vec<String>,
    pub stats_path: Option<PathBuf>,
}

impl CommandSpec {
    pub fn new<P, I, S>(program: P, args: I) -> Self
    where
        P: Into<PathBuf>,
        I: IntoIterator<Item = S>,
        S: Into<String>,
    {
        Self {
            program: program.into(),
            args: args.into_iter().map(Into::into).collect(),
            stats_path: None,
        }
    }
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ExitInfo {
    pub code: Option<i32>,
    pub success: bool,
}

#[derive(Clone, Debug, Serialize)]
#[serde(tag = "type", content = "payload", rename_all = "lowercase")]
pub enum ProcessEvent {
    Telemetry(TelemetryEvent),
    Stats(StatsView),
    Exited(ExitInfo),
    State(ConnectionSnapshot),
}

#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ProcessMessage {
    pub session_id: u64,
    #[serde(flatten)]
    pub event: ProcessEvent,
    pub connection: ConnectionSnapshot,
}

#[derive(Debug, Error)]
pub enum ProcessError {
    #[error("ppp 进程已在运行中")]
    AlreadyRunning,
    #[error("无法启动 ppp: {0}")]
    Spawn(#[from] std::io::Error),
}

type Emitter = Arc<dyn Fn(ProcessMessage) + Send + Sync + 'static>;
type EventSink = Arc<dyn Fn(ProcessEvent) + Send + Sync + 'static>;

/// Clones share the same child; `stop` can then run without holding the
/// caller's lock on the manager.
#[derive(Clone)]
pub struct ProcessManager {
    child: Arc<Mutex<Option<Child>>>,
    emit: Emitter,
    snapshot: Arc<Mutex<ConnectionSnapshot>>,
}

impl ProcessManager {
    pub fn new<F>(emit: F) -> Self
    where
        F: Fn(ProcessMessage) + Send + Sync + 'static,
    {
        Self {
            child: Arc::new(Mutex::new(None)),
            emit: Arc::new(emit),
            snapshot: Arc::new(Mutex::new(ConnectionSnapshot::default())),
        }
    }

    pub fn is_running(&self) -> bool {
        self.child.lock().expect("process lock poisoned").is_some()
    }

    pub fn snapshot(&self) -> ConnectionSnapshot {
        self.snapshot
            .lock()
            .expect("snapshot lock poisoned")
            .clone()
    }

    pub fn start(&mut self, spec: CommandSpec) -> Result<u32, ProcessError> {
        let mut slot = self.child.lock().expect("process lock poisoned");
        if slot.is_some() {
            return Err(ProcessError::AlreadyRunning);
        }
        let mut command = Command::new(&spec.program);
        command
            .args(&spec.args)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::piped());
        #[cfg(windows)]
        {
            use std::os::windows::process::CommandExt;
            command.creation_flags(0x0800_0000);
        }
        let mut child = command.spawn()?;
        let pid = child.id();
        let session_id = {
            let mut snapshot = self.snapshot.lock().expect("snapshot lock poisoned");
            let session_id = snapshot.session_id + 1;
            *snapshot = ConnectionSnapshot {
                session_id,
                pid: Some(pid),
                status: "starting".into(),
                phase: "starting".into(),
                ..ConnectionSnapshot::default()
            };
            session_id
        };
        let snapshot = Arc::clone(&self.snapshot);
        let emitter = Arc::clone(&self.emit);
        let send: EventSink = Arc::new(move |event| {
            let mut current = snapshot.lock().expect("snapshot lock poisoned");
            if current.session_id != session_id {
                return;
            }
            current.apply(&event);
            let connection = current.clone();
            drop(current);
            emitter(ProcessMessage {
                session_id,
                event,
                connection,
            });
        });
        send(ProcessEvent::State(self.snapshot()));
        if let Some(stderr) = child.stderr.take() {
            let emit = Arc::clone(&send);
            thread::spawn(move || {
                for line in BufReader::new(stderr).lines().map_while(Result::ok) {
                    emit(ProcessEvent::Telemetry(classify_line(&line)));
                }
            });
        }
        *slot = Some(child);
        drop(slot);
        monitor_child(Arc::clone(&self.child), send, spec.stats_path, pid);
        Ok(pid)
    }

    pub fn stop(&mut self) -> Result<(), ProcessError> {
        let pid = match self.child.lock().expect("process lock poisoned").as_ref() {
            Some(child) => child.id(),
            None => return Ok(()),
        };
        let state = {
            let mut state = self.snapshot.lock().expect("snapshot lock poisoned");
            state.requested_stop = true;
            state.status = "stopping".into();
            state.clone()
        };
        (self.emit)(ProcessMessage {
            session_id: state.session_id,
            event: ProcessEvent::State(state.clone()),
            connection: state,
        });
        // The kernel rolls back routes, DNS and the adapter on shutdown; give it
        // time to finish before falling back to a hard kill.
        let grace = if request_graceful_stop(pid) {
            GRACEFUL_STOP_TIMEOUT
        } else {
            Duration::ZERO
        };
        let deadline = Instant::now() + grace;
        // Only this pid is ours to stop: a new session may start once it exits.
        let owns = |slot: &Option<Child>| slot.as_ref().is_some_and(|child| child.id() == pid);
        while Instant::now() < deadline {
            if !owns(&self.child.lock().expect("process lock poisoned")) {
                return Ok(());
            }
            thread::sleep(Duration::from_millis(25));
        }
        let exit = {
            let mut slot = self.child.lock().expect("process lock poisoned");
            match slot.as_mut().filter(|child| child.id() == pid) {
                Some(child) => {
                    child.kill()?;
                    child.wait()?;
                    slot.take();
                    true
                }
                None => false,
            }
        };
        if exit {
            let event = ProcessEvent::Exited(ExitInfo {
                code: None,
                success: true,
            });
            let connection = {
                let mut snapshot = self.snapshot.lock().expect("snapshot lock poisoned");
                snapshot.apply(&event);
                snapshot.clone()
            };
            (self.emit)(ProcessMessage {
                session_id: connection.session_id,
                event,
                connection,
            });
        }
        Ok(())
    }
}

fn monitor_child(
    child: Arc<Mutex<Option<Child>>>,
    emit: EventSink,
    stats_path: Option<PathBuf>,
    pid: u32,
) {
    thread::spawn(move || {
        let mut offset = 0;
        let mut pending = String::new();
        let mut sampler = StatsSampler::default();
        loop {
            if !child
                .lock()
                .expect("process lock poisoned")
                .as_ref()
                .is_some_and(|child| child.id() == pid)
            {
                break;
            }
            if let Some(path) = &stats_path {
                read_stats(path, &mut offset, &mut pending, &mut sampler, &emit);
            }
            let exit = {
                let mut slot = child.lock().expect("process lock poisoned");
                if !slot.as_ref().is_some_and(|child| child.id() == pid) {
                    break;
                }
                match slot
                    .as_mut()
                    .and_then(|process| process.try_wait().ok())
                    .flatten()
                {
                    Some(status) => {
                        slot.take();
                        Some(ExitInfo {
                            code: status.code(),
                            success: status.success(),
                        })
                    }
                    None => {
                        if slot.is_none() {
                            break;
                        }
                        None
                    }
                }
            };
            if let Some(exit) = exit {
                if let Some(path) = &stats_path {
                    read_stats(path, &mut offset, &mut pending, &mut sampler, &emit);
                }
                emit(ProcessEvent::Exited(exit));
                break;
            }
            thread::sleep(Duration::from_millis(50));
        }
    });
}

fn read_stats(
    path: &PathBuf,
    offset: &mut u64,
    pending: &mut String,
    sampler: &mut StatsSampler,
    emit: &EventSink,
) {
    let Ok(mut file) = std::fs::File::open(path) else {
        return;
    };
    let Ok(metadata) = file.metadata() else {
        return;
    };
    if metadata.len() < *offset {
        *offset = 0;
        pending.clear();
    }
    if file.seek(SeekFrom::Start(*offset)).is_err() {
        return;
    }
    let mut chunk = String::new();
    if file.read_to_string(&mut chunk).is_err() {
        return;
    }
    *offset += chunk.len() as u64;
    pending.push_str(&chunk);
    while let Some(index) = pending.find('\n') {
        let line: String = pending.drain(..=index).collect();
        if let Ok(view) = sampler.consume_line(line.trim()) {
            emit(ProcessEvent::Stats(view));
        }
    }
}

const GRACEFUL_STOP_TIMEOUT: Duration = Duration::from_secs(5);

/// Delivers CTRL_C_EVENT to the kernel's hidden console. `taskkill` without
/// `/F` only posts WM_CLOSE, which a windowless console process never sees.
#[cfg(windows)]
fn request_graceful_stop(pid: u32) -> bool {
    #[link(name = "kernel32")]
    extern "system" {
        fn AttachConsole(process_id: u32) -> i32;
        fn FreeConsole() -> i32;
        fn GenerateConsoleCtrlEvent(ctrl_event: u32, process_group_id: u32) -> i32;
        fn SetConsoleCtrlHandler(handler: Option<unsafe extern "system" fn(u32) -> i32>, add: i32) -> i32;
    }
    const CTRL_C_EVENT: u32 = 0;
    static CONSOLE: Mutex<()> = Mutex::new(());
    let _guard = CONSOLE.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    unsafe {
        FreeConsole();
        if AttachConsole(pid) == 0 {
            return false;
        }
        // Ignore the event in this process. It stays ignored: the GUI never
        // handles Ctrl+C, and restoring it early could race the delivery.
        SetConsoleCtrlHandler(None, 1);
        let sent = GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0) != 0;
        FreeConsole();
        sent
    }
}

#[cfg(unix)]
fn request_graceful_stop(pid: u32) -> bool {
    unsafe { libc::kill(pid as i32, libc::SIGTERM) == 0 }
}
