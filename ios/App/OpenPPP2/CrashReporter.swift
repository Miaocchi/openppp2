import Foundation
import KSCrashRecording

enum CrashReporter {
    enum ProcessKind: String, CaseIterable, Codable {
        case app = "app"
        case packetTunnel = "packet-tunnel"

        var displayName: String {
            switch self {
            case .app:
                return "App"
            case .packetTunnel:
                return "VPN 扩展"
            }
        }
    }

    struct StoreSnapshot: Codable {
        let process: ProcessKind
        let reportIDs: [Int64]

        var reportCount: Int {
            reportIDs.count
        }
    }

    struct PendingReport {
        let process: ProcessKind
        let id: Int64
    }

    private static let appGroupIdentifier = "group.com.haochengwu.openppp2"
    private static let maxReportCount = 12
    private static var installed = false
    private static let queue = DispatchQueue(label: "openppp2.crash-reporter")

    static func install(process: ProcessKind) {
        queue.sync {
            guard !installed else { return }

            let config = KSCrashConfiguration()
            config.installPath = installPath(for: process).path
            config.reportStoreConfiguration.appName = appName(for: process)
            config.reportStoreConfiguration.reportsPath = reportsPath(for: process).path
            config.reportStoreConfiguration.maxReportCount = maxReportCount
            config.reportStoreConfiguration.reportCleanupPolicy = .never
            config.monitors = [.machException, .signal, .cppException, .nsException]
            config.enableQueueNameSearch = false
            config.enableMemoryIntrospection = false
            config.enableSwapCxaThrow = true
            config.userInfoJSON = [
                "process": process.rawValue,
                "bundleIdentifier": Bundle.main.bundleIdentifier ?? "",
                "bundleVersion": Bundle.main.object(forInfoDictionaryKey: "CFBundleVersion") as? String ?? "",
                "shortVersion": Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? ""
            ]

            do {
                try KSCrash.shared.install(with: config)
                installed = true
            } catch {
                NSLog("OpenPPP2 KSCrash install failed: %@", error.localizedDescription)
            }
        }
    }

    static var pendingReportCount: Int {
        storeSnapshots.reduce(0) { $0 + $1.reportCount }
    }

    static var pendingReports: [PendingReport] {
        storeSnapshots.flatMap { snapshot in
            snapshot.reportIDs.map { PendingReport(process: snapshot.process, id: $0) }
        }
    }

    static var storeSnapshots: [StoreSnapshot] {
        let readableProcesses: [ProcessKind] = isSharedContainerAvailable ? ProcessKind.allCases : [.app]
        return readableProcesses.map { storeSnapshot(for: $0) }
    }

    static var crashedLastLaunch: Bool {
        KSCrash.shared.crashedLastLaunch
    }

    static func deleteAllReports() {
        let writableProcesses: [ProcessKind] = isSharedContainerAvailable ? ProcessKind.allCases : [.app]
        writableProcesses.forEach { process in
            reportStore(for: process)?.deleteAllReports()
        }
    }

    static func deleteReports(for process: ProcessKind) {
        reportStore(for: process)?.deleteAllReports()
    }

    static func pendingReportsSummary() -> String {
        pendingReportsSummary(for: storeSnapshots)
    }

    static func pendingReportsSummary(for snapshots: [StoreSnapshot]) -> String {
        let count = snapshots.reduce(0) { $0 + $1.reportCount }
        if count == 0 {
            return crashedLastLaunch ? "上次启动崩溃，报告正在生成" : "没有待上传报告"
        }

        let processSummary = snapshots
            .filter { $0.reportCount > 0 }
            .map { "\($0.process.displayName) \($0.reportCount)" }
            .joined(separator: " / ")
        return processSummary.isEmpty ? "\(count) 个待上传报告" : "\(count) 个待上传报告（\(processSummary)）"
    }

    static func storageDescription() -> String {
        if isSharedContainerAvailable {
            return "App Group 共享目录"
        }
        return "App 本地缓存；VPN 扩展需在已连接时读取"
    }

    static var isSharedContainerAvailable: Bool {
        sharedContainerURL != nil
    }

    static func storeSnapshot(for process: ProcessKind) -> StoreSnapshot {
        let reportIDs = reportStore(for: process)?.reportIDs.map(\.int64Value) ?? []
        return StoreSnapshot(process: process, reportIDs: reportIDs)
    }

    static func encodedStoreSnapshot(for process: ProcessKind) -> Data? {
        try? JSONEncoder().encode(storeSnapshot(for: process))
    }

    static func decodedStoreSnapshot(from data: Data?) -> StoreSnapshot? {
        guard let data else { return nil }
        return try? JSONDecoder().decode(StoreSnapshot.self, from: data)
    }

    private static func reportStore(for process: ProcessKind) -> CrashReportStore? {
        let configuration = CrashReportStoreConfiguration()
        configuration.appName = appName(for: process)
        configuration.reportsPath = reportsPath(for: process).path
        configuration.maxReportCount = maxReportCount
        configuration.reportCleanupPolicy = .never
        return try? CrashReportStore(configuration: configuration)
    }

    private static func installPath(for process: ProcessKind) -> URL {
        if let container = sharedContainerURL {
            return container.appendingPathComponent("KSCrash/\(process.rawValue)", isDirectory: true)
        }

        let caches = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSTemporaryDirectory(), isDirectory: true)
        return caches.appendingPathComponent("KSCrash/\(process.rawValue)", isDirectory: true)
    }

    private static func reportsPath(for process: ProcessKind) -> URL {
        installPath(for: process).appendingPathComponent("Reports", isDirectory: true)
    }

    private static func appName(for process: ProcessKind) -> String {
        "OpenPPP2-\(process.rawValue)"
    }

    private static var sharedContainerURL: URL? {
        FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: appGroupIdentifier)
    }
}
