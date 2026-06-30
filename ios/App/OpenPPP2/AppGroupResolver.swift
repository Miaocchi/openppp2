import Foundation

enum AppGroupResolver {
    static func resolve(configured: String?, bundleIdentifier: String?, defaultGroup: String = "group.openppp2") -> String {
        if let configured {
            let trimmed = configured.trimmingCharacters(in: .whitespacesAndNewlines)
            if !trimmed.isEmpty {
                return trimmed
            }
        }
        if let bundleIdentifier, !bundleIdentifier.isEmpty {
            return "group." + bundleIdentifier
        }
        return defaultGroup
    }
}

enum SettingsPresentation {
    enum Section: Int, CaseIterable {
        case appStatus
        case diagnostics
        case telemetry
        case system
        case destructive

        var title: String {
            switch self {
            case .appStatus:
                return "应用状态"
            case .diagnostics:
                return "诊断"
            case .telemetry:
                return "遥测上传"
            case .system:
                return "系统"
            case .destructive:
                return "危险操作"
            }
        }

        var rows: [Row] {
            switch self {
            case .appStatus:
                return [.vpnStatus]
            case .diagnostics:
                return [.debugPanel, .crashReports]
            case .telemetry:
                return [.telemetryUpload]
            case .system:
                return [.openSystemSettings]
            case .destructive:
                return [.resetProfiles]
            }
        }
    }

    enum Row: Equatable {
        case vpnStatus
        case debugPanel
        case crashReports
        case telemetryUpload
        case openSystemSettings
        case resetProfiles

        var title: String {
            switch self {
            case .vpnStatus:
                return "VPN 状态"
            case .debugPanel:
                return "调试面板"
            case .crashReports:
                return "崩溃收集"
            case .telemetryUpload:
                return "上传到 OpenTelemetry"
            case .openSystemSettings:
                return "打开系统设置"
            case .resetProfiles:
                return "清空服务器"
            }
        }

        var usesDisclosure: Bool {
            self != .debugPanel
        }
    }

    static func telemetrySummary(uploadEnabled: Bool, destinationName: String, endpoint: String) -> String {
        guard uploadEnabled else {
            return "关闭"
        }
        let trimmedEndpoint = endpoint.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmedEndpoint.isEmpty else {
            return destinationName
        }
        return "\(destinationName) · \(trimmedEndpoint)"
    }
}
