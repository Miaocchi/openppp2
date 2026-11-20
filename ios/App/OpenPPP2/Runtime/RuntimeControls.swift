import Foundation

public enum RuntimeConnectionAction: Equatable, Sendable {
    case start, cancel, stop, retry, forceStop, checkStatus, none
}

public struct RuntimeControlState: Equatable, Sendable {
    public var action: RuntimeConnectionAction
    public var buttonEnabled: Bool
    public var buttonTitleKey: String
    public var statusTitleKey: String
    public var detailKey: String
    public var configEditable: Bool
    public var isConnected: Bool
    public var isBusy: Bool

    public init(
        action: RuntimeConnectionAction,
        buttonEnabled: Bool,
        buttonTitleKey: String,
        statusTitleKey: String,
        detailKey: String,
        configEditable: Bool,
        isConnected: Bool = false,
        isBusy: Bool = false
    ) {
        self.action = action
        self.buttonEnabled = buttonEnabled
        self.buttonTitleKey = buttonTitleKey
        self.statusTitleKey = statusTitleKey
        self.detailKey = detailKey
        self.configEditable = configEditable
        self.isConnected = isConnected
        self.isBusy = isBusy
    }
}

public func controlsFor(
    _ phase: RuntimePhase,
    stopTakingTooLong: Bool = false
) -> RuntimeControlState {
    switch phase {
    case .idle:
        return RuntimeControlState(action: .start, buttonEnabled: true, buttonTitleKey: "home.connect", statusTitleKey: "home.notConnected", detailKey: "home.ready", configEditable: true)
    case .starting, .preparingHost, .connecting, .handshaking, .applyingPolicy:
        return RuntimeControlState(action: .cancel, buttonEnabled: true, buttonTitleKey: "common.cancel", statusTitleKey: "home.connecting", detailKey: "home.vpnStarting", configEditable: false, isBusy: true)
    case .connected:
        return RuntimeControlState(action: .stop, buttonEnabled: true, buttonTitleKey: "home.stop", statusTitleKey: "home.connected", detailKey: "", configEditable: false, isConnected: true)
    case .reconnecting:
        return RuntimeControlState(action: .stop, buttonEnabled: true, buttonTitleKey: "home.stop", statusTitleKey: "home.reconnecting", detailKey: "home.networkChanged", configEditable: false, isBusy: true)
    case .stopping:
        return RuntimeControlState(action: .none, buttonEnabled: false, buttonTitleKey: "home.stop", statusTitleKey: "home.disconnecting", detailKey: stopTakingTooLong ? "home.stopTakingTooLong" : "home.stopping", configEditable: false, isBusy: true)
    case .failed:
        return RuntimeControlState(action: .retry, buttonEnabled: true, buttonTitleKey: "home.retry", statusTitleKey: "home.connectFailed", detailKey: "home.ready", configEditable: true)
    case .unknown:
        return RuntimeControlState(action: .forceStop, buttonEnabled: true, buttonTitleKey: "home.forceStop", statusTitleKey: "vpn.unknown", detailKey: "settings.section.diagnostics", configEditable: false)
    }
}

enum SystemSessionStatus {
    case inactive, connected, transitioning, unavailable
}

/// A system session can veto Start, but cannot establish runtime readiness.
/// A missing snapshot is different from a runtime reporting Unknown.
func homeControls(runtime: RuntimePhase?, system: SystemSessionStatus, snapshotCurrent: Bool = true, stopTakingTooLong: Bool = false) -> RuntimeControlState {
    if let runtime, !snapshotCurrent {
        // An unreadable/out-of-order observation cannot confirm readiness. Keep
        // a previously known stop/cancel command, but never start from it.
        let previous = controlsFor(runtime)
        var unknown = controlsFor(.unknown)
        if previous.action == .stop || previous.action == .cancel {
            unknown.action = previous.action
            unknown.buttonTitleKey = previous.buttonTitleKey
        }
        return unknown
    }
    if system == .unavailable && (runtime == nil || runtime == .idle || runtime == .failed) {
        return RuntimeControlState(action: .checkStatus, buttonEnabled: true, buttonTitleKey: "home.checkAgain", statusTitleKey: "vpn.unknown", detailKey: "", configEditable: false)
    }
    let phase = runtime ?? (system == .inactive ? .idle : .unknown)
    var controls = controlsFor(phase, stopTakingTooLong: stopTakingTooLong)
    if system != .inactive && controls.configEditable {
        return controlsFor(.unknown)
    }
    if controls.isConnected && system != .connected {
        controls.isConnected = false
        controls.statusTitleKey = "vpn.unknown"
        controls.detailKey = "home.statusMismatch"
    }
    return controls
}
