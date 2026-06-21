import Foundation
import NetworkExtension

final class PacketTunnelProvider: NEPacketTunnelProvider {
    private var adapter: OpenPPP2PacketTunnelAdapter?
    private var lastOptions = PacketTunnelOptions()

    override func startTunnel(
        options: [String: NSObject]?,
        completionHandler: @escaping (Error?) -> Void
    ) {
        CrashReporter.install(process: .packetTunnel)

        let providerConfiguration = (protocolConfiguration as? NETunnelProviderProtocol)?.providerConfiguration
        let launchOptions = Self.readOptions(from: providerConfiguration)
        let configJson = providerConfiguration?["configJson"] as? String ?? "{}"
        lastOptions = launchOptions
        let preparedConfigJson = Self.preparedConfigJson(configJson)
        let settings = NEPacketTunnelNetworkSettings(tunnelRemoteAddress: launchOptions.gateway)

        let ipv4 = NEIPv4Settings(
            addresses: [launchOptions.tunIp],
            subnetMasks: [launchOptions.tunMask]
        )
        ipv4.includedRoutes = [
            NEIPv4Route(destinationAddress: launchOptions.route, subnetMask: Self.mask(prefix: launchOptions.routePrefix))
        ]
        ipv4.excludedRoutes = Self.excludedRoutes(from: launchOptions.bypassIpList)
        settings.ipv4Settings = ipv4
        settings.mtu = NSNumber(value: launchOptions.mtu)

        let dnsServers: [String] = [launchOptions.dns1, launchOptions.dns2]
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }
        if !dnsServers.isEmpty {
            settings.dnsSettings = NEDNSSettings(servers: dnsServers)
        }

        setTunnelNetworkSettings(settings) { [weak self] error in
            if let error {
                completionHandler(error)
                return
            }

            guard let self else {
                completionHandler(nil)
                return
            }

            let adapter = OpenPPP2PacketTunnelAdapter(flow: self.packetFlow)
            guard adapter.start(configJson: preparedConfigJson, options: launchOptions) else {
                completionHandler(NSError(
                    domain: "OpenPPP2PacketTunnel",
                    code: 1,
                    userInfo: [NSLocalizedDescriptionKey: "Unable to start OpenPPP2 tunnel engine: \(openPPP2LastErrorText())"]
                ))
                return
            }

            self.adapter = adapter
            completionHandler(nil)
        }
    }

    override func stopTunnel(
        with reason: NEProviderStopReason,
        completionHandler: @escaping () -> Void
    ) {
        adapter?.stop()
        adapter = nil
        completionHandler()
    }

    override func handleAppMessage(_ messageData: Data, completionHandler: ((Data?) -> Void)?) {
        guard let command = String(data: messageData, encoding: .utf8) else {
            completionHandler?(nil)
            return
        }

        switch command {
        case "stats":
            completionHandler?(adapter?.statisticsJson().data(using: .utf8))
        case "linkState":
            let state = adapter?.linkState() ?? 2
            completionHandler?("\(state)".data(using: .utf8))
        case "crashReports":
            completionHandler?(CrashReporter.encodedStoreSnapshot(for: .packetTunnel))
        case "deleteCrashReports":
            CrashReporter.deleteReports(for: .packetTunnel)
            completionHandler?(CrashReporter.encodedStoreSnapshot(for: .packetTunnel))
        default:
            completionHandler?(nil)
        }
    }

    private static func readOptions(from providerConfiguration: [String: Any]?) -> PacketTunnelOptions {
        guard let raw = providerConfiguration?["optionsJson"] as? String,
              let data = raw.data(using: .utf8),
              let options = try? JSONDecoder().decode(PacketTunnelOptions.self, from: data)
        else {
            return PacketTunnelOptions()
        }
        return options
    }

    private static func preparedConfigJson(_ rawJson: String) -> String {
        let rootDirectory = prepareRuntimeRoot()
        guard let data = rawJson.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data),
              var root = object as? [String: Any]
        else {
            return rawJson
        }

        var geo = root["geo-rules"] as? [String: Any] ?? [:]
        if let rulesDirectory = rootDirectory?.appendingPathComponent("rules", isDirectory: true) {
            geo["geoip-dat"] = rulesDirectory.appendingPathComponent("GeoIP.dat").path
            geo["geosite-dat"] = rulesDirectory.appendingPathComponent("GeoSite.dat").path
        }
        root["geo-rules"] = geo

        guard JSONSerialization.isValidJSONObject(root),
              let output = try? JSONSerialization.data(withJSONObject: root, options: [.sortedKeys]),
              let text = String(data: output, encoding: .utf8)
        else {
            return rawJson
        }
        return text
    }

    @discardableResult
    private static func prepareRuntimeRoot() -> URL? {
        let fileManager = FileManager.default
        guard let documents = fileManager.urls(for: .documentDirectory, in: .userDomainMask).first else {
            return nil
        }

        let rulesDirectory = documents.appendingPathComponent("rules", isDirectory: true)
        do {
            try fileManager.createDirectory(at: rulesDirectory, withIntermediateDirectories: true)
            try copyBundledRule(named: "geoip", outputName: "GeoIP.dat", to: rulesDirectory)
            try copyBundledRule(named: "geosite", outputName: "GeoSite.dat", to: rulesDirectory)
        } catch {
            NSLog("OpenPPP2 prepareRuntimeRoot failed: %@", error.localizedDescription)
        }
        return documents
    }

    private static func copyBundledRule(named resourceName: String, outputName: String, to directory: URL) throws {
        guard let source = Bundle.main.url(forResource: resourceName, withExtension: "dat", subdirectory: "rules") else {
            return
        }

        let destination = directory.appendingPathComponent(outputName)
        let attributes = try? FileManager.default.attributesOfItem(atPath: destination.path)
        if let type = attributes?[.type] as? FileAttributeType,
           type == .typeRegular,
           let size = attributes?[.size] as? NSNumber,
           size.int64Value > 0 {
            return
        }

        if FileManager.default.fileExists(atPath: destination.path) {
            try FileManager.default.removeItem(at: destination)
        }
        try FileManager.default.copyItem(at: source, to: destination)
    }

    private static func excludedRoutes(from text: String) -> [NEIPv4Route] {
        text
            .components(separatedBy: .newlines)
            .compactMap { line -> NEIPv4Route? in
                let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
                guard !trimmed.isEmpty else { return nil }
                let parts = trimmed.split(separator: "/", maxSplits: 1).map(String.init)
                let address = parts[0]
                let prefix = parts.count == 2 ? (Int(parts[1]) ?? 32) : 32
                return NEIPv4Route(destinationAddress: address, subnetMask: mask(prefix: prefix))
            }
    }

    private static func mask(prefix: Int) -> String {
        let clamped = max(0, min(prefix, 32))
        let value = clamped == 0 ? UInt32(0) : UInt32.max << UInt32(32 - clamped)
        return [24, 16, 8, 0]
            .map { String((value >> UInt32($0)) & 0xff) }
            .joined(separator: ".")
    }
}
