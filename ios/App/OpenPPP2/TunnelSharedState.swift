import Foundation

/// Cross-process tunnel liveness and diagnostics via App Group (mirrors Android
/// `openppp2-linkstate.txt` + `PppStateStore`).
enum TunnelSharedState {
    static var appGroupIdentifier: String {
        AppGroupResolver.resolve(
            configured: Bundle.main.object(forInfoDictionaryKey: "OpenPPP2AppGroupIdentifier") as? String,
            bundleIdentifier: Bundle.main.bundleIdentifier
        )
    }
    static let heartbeatStaleMilliseconds: Int64 = 30_000
    static let connectWatchdogMaxSeconds = 180

    private static let linkStateFileName = "openppp2-linkstate.txt"
    private static let diagnosticsFileName = "openppp2-shared-diagnostics.json"
    private static let queue = DispatchQueue(label: "openppp2.tunnel-shared-state")

    static var isSharedContainerAvailable: Bool {
        containerURL() != nil
    }

    static var shouldUseSharedHeartbeat: Bool {
        isSharedContainerAvailable
    }

    static func beginSession() {
        writeLinkStateHeartbeat(6)
    }

    static func writeLinkStateHeartbeat(_ linkState: Int) {
        queue.async {
            guard let url = linkStateURL() else { return }
            try? "\(linkState)\n".write(to: url, atomically: true, encoding: .utf8)
        }
    }

    static func heartbeatAgeMs() -> Int64 {
        guard shouldUseSharedHeartbeat,
              let url = linkStateURL(),
              let attributes = try? FileManager.default.attributesOfItem(atPath: url.path),
              let modified = attributes[.modificationDate] as? Date
        else {
            return -1
        }
        return Int64(Date().timeIntervalSince(modified) * 1000)
    }

    static func isExtensionAlive() -> Bool {
        guard shouldUseSharedHeartbeat else { return true }
        let age = heartbeatAgeMs()
        return age >= 0 && age <= heartbeatStaleMilliseconds
    }

    static func readLinkStateIfAlive() -> Int? {
        guard shouldUseSharedHeartbeat,
              isExtensionAlive(),
              let url = linkStateURL(),
              let text = try? String(contentsOf: url, encoding: .utf8)
        else {
            return nil
        }

        let line = text
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .components(separatedBy: .newlines)
            .first ?? ""
        return Int(line)
    }

    static func writeDiagnosticsJson(_ json: String) {
        queue.async {
            guard shouldUseSharedHeartbeat,
                  let url = diagnosticsURL(),
                  let data = json.data(using: .utf8)
            else {
                return
            }
            try? data.write(to: url, options: .atomic)
        }
    }

    static func readDiagnosticsJson() -> String? {
        guard shouldUseSharedHeartbeat,
              let url = diagnosticsURL(),
              let text = try? String(contentsOf: url, encoding: .utf8),
              !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
        else {
            return nil
        }
        return text
    }

    static func clearSession() {
        queue.sync {
            guard let base = containerURL() else { return }
            for name in [linkStateFileName, diagnosticsFileName] {
                let url = base.appendingPathComponent(name)
                if FileManager.default.fileExists(atPath: url.path) {
                    try? FileManager.default.removeItem(at: url)
                }
            }
        }
    }

    private static func containerURL() -> URL? {
        FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: appGroupIdentifier)
    }

    private static func linkStateURL() -> URL? {
        containerURL()?.appendingPathComponent(linkStateFileName)
    }

    private static func diagnosticsURL() -> URL? {
        containerURL()?.appendingPathComponent(diagnosticsFileName)
    }
}
