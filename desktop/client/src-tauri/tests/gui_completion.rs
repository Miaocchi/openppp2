use openppp2_client_desktop::{
    config::{apply_network_overrides, build_node_config_with_base},
    connection::ConnectionSnapshot,
    kernel, network,
    process::{ExitInfo, ProcessEvent},
    subscription::parse_subscription,
};
use serde_json::json;

#[test]
fn compact_config_missing_sections_does_not_panic_and_overrides_preserve_unknown_fields() {
    let document=parse_subscription(br#"{"type":"openppp2-subscription","version":1,"nodes":[{"id":"a","server":"ppp://127.0.0.1:1234/","key":{"protocol-key":"secret"}}]}"#).unwrap();
    let mut config =
        build_node_config_with_base(&document.nodes[0], Some(r#"{"future":42}"#)).unwrap();
    assert_eq!(config["client"]["server"], "ppp://127.0.0.1:1234/");
    apply_network_overrides(&mut config, &json!({"client":{"http-proxy":{"port":9090}}})).unwrap();
    assert_eq!(config["future"], 42);
    assert_eq!(config["client"]["http-proxy"]["bind"], "127.0.0.1");
    assert!(build_node_config_with_base(&document.nodes[0], Some(r#"{"client":false}"#)).is_err());
    assert!(
        network::validate(&json!({"client":{"http-proxy":{"bind":"0.0.0.0","port":8080}}}))
            .is_err()
    );
    assert!(
        network::validate(&json!({"client":{"http-proxy":{"bind":"127.0.0.1","port":0}}})).is_err()
    );
    let redacted = kernel::redact(config);
    assert_eq!(redacted["key"]["protocol-key"], "[redacted]");
}

#[test]
fn intentional_disconnect_is_not_an_error() {
    let mut snapshot = ConnectionSnapshot {
        pid: Some(42),
        requested_stop: true,
        status: "stopping".into(),
        ..Default::default()
    };
    snapshot.apply(&ProcessEvent::Exited(ExitInfo {
        code: Some(1),
        success: false,
    }));
    assert_eq!(snapshot.status, "disconnected");
    assert!(snapshot.pid.is_none());
}

#[test]
fn duplicate_subscription_node_ids_are_rejected() {
    let body = json!({"type":"openppp2-subscription","version":1,"nodes":[{"id":"a","config":{}},{"id":"a","config":{}}]});
    assert!(parse_subscription(&serde_json::to_vec(&body).unwrap()).is_err());
}
