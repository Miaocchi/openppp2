use serde_json::Value;
use std::collections::BTreeMap;
use thiserror::Error;

#[derive(Debug, Error)]
pub enum LaunchOptionsError {
    #[error("节点启动参数必须是 JSON object")]
    InvalidNodeOptions,
    #[error("启动参数 {0} 必须是字符串")]
    InvalidString(String),
    #[error("启动参数 {0} 必须是布尔值")]
    InvalidBoolean(String),
    #[error("启动参数 mux 必须是 0 到 65535 之间的整数")]
    InvalidMux,
    #[error("启动参数 muxMode 必须是 compat、flow、balance 或 stripe")]
    InvalidMuxMode,
    #[error("启动参数 tcpStack 必须是 native、lwip 或 xtcp")]
    InvalidTcpStack,
    #[error("启动参数 tunIpv6 必须是 IPv6 地址")]
    InvalidTunIpv6,
}

pub fn merge_launch_options(
    global: &BTreeMap<String, Value>,
    node: Option<&Value>,
) -> Result<BTreeMap<String, Value>, LaunchOptionsError> {
    let mut merged = global.clone();
    if let Some(node) = node {
        let values = node
            .as_object()
            .ok_or(LaunchOptionsError::InvalidNodeOptions)?;
        for (key, value) in values {
            merged.insert(key.clone(), value.clone());
        }
    }
    Ok(merged)
}

pub fn append_launch_args(
    options: &BTreeMap<String, Value>,
    args: &mut Vec<String>,
) -> Result<(), LaunchOptionsError> {
    for (field, flag) in [
        ("tunIp", "--tun-ip"),
        ("tunMask", "--tun-mask"),
        ("gateway", "--tun-gw"),
    ] {
        if let Some(value) = optional_string(options, field)? {
            args.push(format!("{flag}={value}"));
        }
    }

    let dns = [
        optional_string(options, "dns1")?,
        optional_string(options, "dns2")?,
    ]
    .into_iter()
    .flatten()
    .collect::<Vec<_>>();
    if !dns.is_empty() {
        args.push(format!("--dns={}", dns.join(",")));
    }

    if let Some(value) = options.get("mux") {
        let mux = value.as_u64().filter(|value| *value <= u16::MAX as u64);
        let mux = mux.ok_or(LaunchOptionsError::InvalidMux)?;
        args.push(format!("--tun-mux={mux}"));
    }
    if let Some(mode) = optional_string(options, "muxMode")? {
        if !["compat", "flow", "balance", "stripe"].contains(&mode) {
            return Err(LaunchOptionsError::InvalidMuxMode);
        }
        args.push(format!("--mux-mode={mode}"));
    }
    if let Some(stack) = optional_string(options, "tcpStack")? {
        if !["native", "lwip", "xtcp"].contains(&stack) {
            return Err(LaunchOptionsError::InvalidTcpStack);
        }
        args.push(format!("--tcp-stack={stack}"));
    }
    if let Some(address) = optional_string(options, "tunIpv6")? {
        // A request hint only; the server owns IPv6 allocation.
        if address.parse::<std::net::Ipv6Addr>().is_err() {
            return Err(LaunchOptionsError::InvalidTunIpv6);
        }
        args.push(format!("--tun-ipv6={address}"));
    }
    for (field, flag) in [
        ("vnet", "--tun-vnet"),
        ("blockQuic", "--block-quic"),
        ("staticMode", "--tun-static"),
        ("muxTurbo", "--mux-mode-turbo"),
    ] {
        if let Some(value) = optional_bool(options, field)? {
            args.push(format!("{flag}={}", if value { "yes" } else { "no" }));
        }
    }
    Ok(())
}

fn optional_string<'a>(
    options: &'a BTreeMap<String, Value>,
    field: &str,
) -> Result<Option<&'a str>, LaunchOptionsError> {
    match options.get(field) {
        None | Some(Value::Null) => Ok(None),
        Some(Value::String(value)) => Ok((!value.trim().is_empty()).then(|| value.trim())),
        Some(_) => Err(LaunchOptionsError::InvalidString(field.into())),
    }
}

fn optional_bool(
    options: &BTreeMap<String, Value>,
    field: &str,
) -> Result<Option<bool>, LaunchOptionsError> {
    match options.get(field) {
        None | Some(Value::Null) => Ok(None),
        Some(Value::Bool(value)) => Ok(Some(*value)),
        Some(_) => Err(LaunchOptionsError::InvalidBoolean(field.into())),
    }
}
