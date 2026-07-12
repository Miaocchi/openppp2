import Foundation
import NetworkExtension

public enum RuntimeConnectionAction: Equatable, Sendable {
    case start
    case cancel
    case stop
    case retry
    case forceStop
    case none
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
    public var diagnosticsAvailable: Bool

    public init(
        action: RuntimeConnectionAction,
        buttonEnabled: Bool,
        buttonTitleKey: String,
        statusTitleKey: String,
        detailKey: String,
        configEditable: Bool,
        isConnected: Bool = false,
        isBusy: Bool = false,
        diagnosticsAvailable: Bool = false
    ) {
        self.action = action
        self.buttonEnabled = buttonEnabled
        self.buttonTitleKey = buttonTitleKey
        self.statusTitleKey = statusTitleKey
        self.detailKey = detailKey
        self.configEditable = configEditable
        self.isConnected = isConnected
        self.isBusy = isBusy
        self.diagnosticsAvailable = diagnosticsAvailable
    }
}

public func controlsFor(_ phase: RuntimePhase) -> RuntimeControlState {
    switch phase {
    case .idle:
        return RuntimeControlState(
            action: .start,
            buttonEnabled: true,
            buttonTitleKey: "home.connect",
            statusTitleKey: "home.notConnected",
            detailKey: "home.ready",
            configEditable: true
        )
    case .starting, .preparingHost, .connecting, .handshaking, .applyingPolicy:
        return RuntimeControlState(
            action: .cancel,
            buttonEnabled: true,
            buttonTitleKey: "common.cancel",
            statusTitleKey: "home.connecting",
            detailKey: "home.vpnStarting",
            configEditable: false,
            isBusy: true
        )
    case .connected:
        return RuntimeControlState(
            action: .stop,
            buttonEnabled: true,
            buttonTitleKey: "home.stop",
            statusTitleKey: "home.connected",
            detailKey: "",
            configEditable: false,
            isConnected: true
        )
    case .reconnecting:
        return RuntimeControlState(
            action: .stop,
            buttonEnabled: true,
            buttonTitleKey: "home.stop",
            statusTitleKey: "home.reconnecting",
            detailKey: "home.networkChanged",
            configEditable: false,
            isBusy: true
        )
    case .stopping:
        return RuntimeControlState(
            action: .none,
            buttonEnabled: false,
            buttonTitleKey: "home.stop",
            statusTitleKey: "home.disconnecting",
            detailKey: "home.stopping",
            configEditable: false,
            isBusy: true
        )
    case .failed:
        return RuntimeControlState(
            action: .retry,
            buttonEnabled: true,
            buttonTitleKey: "home.retry",
            statusTitleKey: "home.connectFailed",
            detailKey: "home.ready",
            configEditable: true
        )
    case .unknown:
        return RuntimeControlState(
            action: .forceStop,
            buttonEnabled: true,
            buttonTitleKey: "home.forceStop",
            statusTitleKey: "vpn.unknown",
            detailKey: "settings.section.diagnostics",
            configEditable: false,
            diagnosticsAvailable: true
        )
    }
}

public func runtimeSnapshot(
    previous: RuntimeSnapshot,
    status: NEVPNStatus,
    linkState: Int
) -> RuntimeSnapshot {
    RuntimeSnapshot(
        generation: previous.generation + 1,
        monotonicMs: nextMonotonicMs(after: previous.monotonicMs),
        phase: runtimePhase(status: status, linkState: linkState)
    )
}

public func runtimeSnapshot(
    previous: RuntimeSnapshot,
    phase: RuntimePhase
) -> RuntimeSnapshot {
    RuntimeSnapshot(
        generation: previous.generation + 1,
        monotonicMs: nextMonotonicMs(after: previous.monotonicMs),
        phase: phase
    )
}

private func nextMonotonicMs(after previous: UInt64) -> UInt64 {
    let now = UInt64(Date().timeIntervalSince1970 * 1000)
    return now <= previous ? previous + 1 : now
}

private func runtimePhase(status: NEVPNStatus, linkState: Int) -> RuntimePhase {
    // ponytail: this collapses NEVPNStatus + linkState until native
    // RuntimeSnapshot JSON streams to mobile; replace with direct snapshot decode.
    switch status {
    case .invalid, .disconnected:
        return .idle
    case .disconnecting:
        return .stopping
    case .reasserting:
        return .reconnecting
    case .connecting, .connected:
        return runtimePhaseForActiveLink(status: status, linkState: linkState)
    @unknown default:
        return .unknown
    }
}

private func runtimePhaseForActiveLink(status: NEVPNStatus, linkState: Int) -> RuntimePhase {
    switch linkState {
    case 0:
        return .connected
    case 2, 3, 6:
        return .preparingHost
    case 4:
        return .reconnecting
    case 5:
        return .handshaking
    default:
        return status == .connected ? .connected : .connecting
    }
}
