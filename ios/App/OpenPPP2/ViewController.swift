import NetworkExtension
import UIKit

final class ViewController: UITabBarController {
    override func viewDidLoad() {
        super.viewDidLoad()

        tabBar.tintColor = .systemBlue
        tabBar.backgroundColor = .systemBackground

        viewControllers = [
            makeTab(HomeViewController(), title: "主页", image: "house", selectedImage: "house.fill"),
            makeTab(OptionsViewController(), title: "启动参数", image: "slider.horizontal.3", selectedImage: "slider.horizontal.3"),
            makeTab(ProfilesViewController(), title: "配置文件", image: "folder", selectedImage: "folder.fill"),
            makeTab(SettingsViewController(), title: "设置", image: "gearshape", selectedImage: "gearshape.fill")
        ]
    }

    private func makeTab(_ root: UIViewController, title: String, image: String, selectedImage: String) -> UIViewController {
        let nav = UINavigationController(rootViewController: root)
        nav.navigationBar.prefersLargeTitles = false
        nav.tabBarItem = UITabBarItem(
            title: title,
            image: UIImage(systemName: image),
            selectedImage: UIImage(systemName: selectedImage)
        )
        return nav
    }
}

// MARK: - Models

struct ConfigSnapshot: Codable, Equatable {
    var timestampMs: Int
    var json: String
}

struct LaunchOptions: Codable, Equatable {
    var tunIp: String = "10.0.0.2"
    var tunMask: String = "255.255.255.0"
    var tunPrefix: Int = 24
    var gateway: String = "10.0.0.1"
    var route: String = "0.0.0.0"
    var routePrefix: Int = 0
    var dns1: String = "8.8.8.8"
    var dns2: String = "1.1.1.1"
    var mtu: Int = 1400
    var mux: Int = 0
    var vnet: Bool = false
    var blockQuic: Bool = true
    var staticMode: Bool = false
    var bypassIpList: String = ""
    var dnsRulesList: String = ""
    var allowLan: Bool = true
    var dnsDomestic: String = "doh.pub"
    var dnsForeign: String = "cloudflare"
    var dnsInterceptUnmatched: Bool = true
    var dnsEcsEnabled: Bool = true
    var dnsEcsOverrideIp: String = ""
    var dnsTlsVerifyPeer: Bool = true
    var dnsStunCandidates: String = "39.107.142.158:3478\n74.125.250.129:19302"
    var geoEnabled: Bool = true
    var geoCountry: String = "cn"
    var geoIpDat: String = "./rules/GeoIP.dat"
    var geoSiteDat: String = "./rules/GeoSite.dat"
}

struct ConfigProfile: Codable, Equatable {
    var id: String
    var name: String
    var subtitle: String
    var flag: String
    var json: String
    var favorite: Bool
    var options: LaunchOptions
    var history: [ConfigSnapshot]

    static let historyLimit = 8

    var serverEndpoint: String? {
        guard
            let data = json.data(using: .utf8),
            let object = try? JSONSerialization.jsonObject(with: data),
            let root = object as? [String: Any],
            let client = root["client"] as? [String: Any],
            let server = client["server"] as? String,
            let url = URL(string: server),
            let host = url.host,
            !host.isEmpty
        else {
            return nil
        }

        if let port = url.port {
            return "\(host):\(port)"
        }
        return host
    }
}

struct VpnStatistics: Equatable {
    var txSpeedBytes: Int = 0
    var rxSpeedBytes: Int = 0
    var inBytes: Int = 0
    var outBytes: Int = 0

    static let empty = VpnStatistics()

    init() {}

    init(jsonText: String, previous: VpnStatistics = .empty) {
        guard let data = jsonText.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data),
              let map = object as? [String: Any]
        else {
            self = previous
            return
        }

        func value(_ keys: [String]) -> Int? {
            for key in keys {
                guard let raw = map[key] else { continue }
                if let number = raw as? NSNumber { return number.intValue }
                if let text = raw as? String, let parsed = Int(text) { return parsed }
            }
            return nil
        }

        let nativeTxSpeed = value(["tx", "txBytes", "outgoing", "outgoingTraffic"]) ?? 0
        let nativeRxSpeed = value(["rx", "rxBytes", "incoming", "incomingTraffic"]) ?? 0
        let nativeInTotal = value(["in", "inBytes", "incomingTotal", "incomingTrafficTotal"])
        let nativeOutTotal = value(["out", "outBytes", "outgoingTotal", "outgoingTrafficTotal"])
        let hasPreviousTotals = previous.inBytes > 0 || previous.outBytes > 0

        inBytes = max(previous.inBytes, nativeInTotal ?? (previous.inBytes + nativeRxSpeed))
        outBytes = max(previous.outBytes, nativeOutTotal ?? (previous.outBytes + nativeTxSpeed))
        rxSpeedBytes = nativeInTotal != nil && hasPreviousTotals ? max(0, inBytes - previous.inBytes) : nativeRxSpeed
        txSpeedBytes = nativeOutTotal != nil && hasPreviousTotals ? max(0, outBytes - previous.outBytes) : nativeTxSpeed
    }
}

// MARK: - Store

final class ProfileStore {
    static let shared = ProfileStore()
    static let didChangeNotification = Notification.Name("OpenPPP2ProfileStoreDidChange")

    private let defaults = UserDefaults.standard
    private let profilesKey = "openppp2_profiles_v2"
    private let activeIdKey = "openppp2_active_profile_id"
    private let debugPanelKey = "openppp2_debug_panel_enabled"
    private let encoder = JSONEncoder()
    private let decoder = JSONDecoder()

    private init() {}

    func profiles() -> [ConfigProfile] {
        ensureSeeded()
        guard let data = defaults.data(forKey: profilesKey),
              let list = try? decoder.decode([ConfigProfile].self, from: data)
        else {
            return []
        }
        return list
    }

    func activeProfile() -> ConfigProfile? {
        let list = profiles()
        guard !list.isEmpty else { return nil }
        let id = defaults.string(forKey: activeIdKey)
        return list.first(where: { $0.id == id }) ?? list.first
    }

    func setActive(_ id: String) {
        defaults.set(id, forKey: activeIdKey)
        emitChange()
    }

    func add(_ profile: ConfigProfile) {
        var list = profiles()
        list.append(profile)
        defaults.set(profile.id, forKey: activeIdKey)
        save(list)
    }

    func update(_ profile: ConfigProfile, snapshot: Bool = true) {
        var list = profiles()
        guard let index = list.firstIndex(where: { $0.id == profile.id }) else {
            add(profile)
            return
        }

        var updated = profile
        let previous = list[index]
        if snapshot, previous.json != profile.json {
            updated.history.insert(
                ConfigSnapshot(timestampMs: Int(Date().timeIntervalSince1970 * 1000), json: previous.json),
                at: 0
            )
            if updated.history.count > ConfigProfile.historyLimit {
                updated.history = Array(updated.history.prefix(ConfigProfile.historyLimit))
            }
        }

        list[index] = updated
        save(list)
    }

    func remove(_ id: String) {
        var list = profiles()
        list.removeAll { $0.id == id }
        if list.isEmpty {
            list = [Self.defaultProfile()]
        }
        if defaults.string(forKey: activeIdKey) == id {
            defaults.set(list[0].id, forKey: activeIdKey)
        }
        save(list)
    }

    func debugPanelEnabled() -> Bool {
        defaults.bool(forKey: debugPanelKey)
    }

    func setDebugPanelEnabled(_ enabled: Bool) {
        defaults.set(enabled, forKey: debugPanelKey)
        emitChange()
    }

    func resetAll() {
        defaults.removeObject(forKey: profilesKey)
        defaults.removeObject(forKey: activeIdKey)
        ensureSeeded()
        emitChange()
    }

    static func defaultProfile() -> ConfigProfile {
        ConfigProfile(
            id: UUID().uuidString,
            name: "Default",
            subtitle: "192.168.0.24:20000",
            flag: "",
            json: defaultJson,
            favorite: false,
            options: LaunchOptions(),
            history: []
        )
    }

    static func effectiveJson(_ profileJson: String, options: LaunchOptions) -> String {
        let source = profileJson.data(using: .utf8) ?? Data()
        let defaultData = defaultJson.data(using: .utf8) ?? Data()
        let object = (try? JSONSerialization.jsonObject(with: source))
            ?? (try? JSONSerialization.jsonObject(with: defaultData))
            ?? [:]
        var root = object as? [String: Any] ?? [:]

        var dns = root["dns"] as? [String: Any] ?? [:]
        var servers = dns["servers"] as? [String: Any] ?? [:]
        if !options.dnsDomestic.isEmpty { servers["domestic"] = options.dnsDomestic }
        if !options.dnsForeign.isEmpty { servers["foreign"] = options.dnsForeign }
        dns["servers"] = servers
        dns["intercept-unmatched"] = options.dnsInterceptUnmatched
        dns["ecs"] = [
            "enabled": options.dnsEcsEnabled,
            "override-ip": options.dnsEcsOverrideIp
        ]
        dns["tls"] = ["verify-peer": options.dnsTlsVerifyPeer]
        dns["stun"] = ["candidates": splitLines(options.dnsStunCandidates)]
        root["dns"] = dns

        var geo = root["geo-rules"] as? [String: Any] ?? [:]
        geo["enabled"] = options.geoEnabled
        geo["country"] = options.geoCountry
        geo["geoip-dat"] = options.geoIpDat
        geo["geosite-dat"] = options.geoSiteDat
        root["geo-rules"] = geo

        var client = root["client"] as? [String: Any] ?? [:]
        let bind = options.allowLan ? "0.0.0.0" : "127.0.0.1"
        var http = client["http-proxy"] as? [String: Any] ?? ["port": 8080]
        var socks = client["socks-proxy"] as? [String: Any] ?? ["port": 1080]
        http["bind"] = bind
        socks["bind"] = bind
        client["http-proxy"] = http
        client["socks-proxy"] = socks
        root["client"] = client

        guard JSONSerialization.isValidJSONObject(root),
              let data = try? JSONSerialization.data(withJSONObject: root, options: [.prettyPrinted, .sortedKeys]),
              let json = String(data: data, encoding: .utf8)
        else {
            return profileJson
        }
        return json
    }

    private static func splitLines(_ value: String) -> [String] {
        value
            .components(separatedBy: .newlines)
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            .filter { !$0.isEmpty }
    }

    private func ensureSeeded() {
        if defaults.data(forKey: profilesKey) != nil { return }
        let seed = Self.defaultProfile()
        save([seed], notify: false)
        defaults.set(seed.id, forKey: activeIdKey)
    }

    private func save(_ profiles: [ConfigProfile], notify: Bool = true) {
        if let data = try? encoder.encode(profiles) {
            defaults.set(data, forKey: profilesKey)
        }
        if notify { emitChange() }
    }

    private func emitChange() {
        NotificationCenter.default.post(name: Self.didChangeNotification, object: self)
    }

    static let defaultJson = """
    {
      "concurrent" : 1,
      "cdn" : [80, 443],
      "key" : {
        "kf" : 154543927,
        "kx" : 128,
        "kl" : 10,
        "kh" : 12,
        "protocol" : "aes-128-cfb",
        "protocol-key" : "N6HMzdUs7IUnYHwq",
        "transport" : "aes-256-cfb",
        "transport-key" : "HWFweXu2g5RVMEpy",
        "masked" : false,
        "plaintext" : false,
        "delta-encode" : false,
        "shuffle-data" : false
      },
      "ip" : { "public" : "0.0.0.0", "interface" : "0.0.0.0" },
      "server" : {
        "node" : 1,
        "log" : "./ppp.log",
        "subnet" : true,
        "mapping" : false,
        "backend" : "",
        "backend-key" : ""
      },
      "tcp" : {
        "inactive" : { "timeout" : 300 },
        "connect" : { "timeout" : 5 },
        "listen" : { "port" : 20000 },
        "turbo" : true,
        "backlog" : 511,
        "fast-open" : true
      },
      "udp" : {
        "inactive" : { "timeout" : 72 },
        "dns" : { "timeout" : 4, "redirect" : "8.8.8.8:53" },
        "listen" : { "port" : 20000 },
        "static" : {
          "keep-alived" : [1, 5],
          "dns" : true,
          "quic" : false,
          "icmp" : true,
          "server" : "127.0.0.1:20000"
        }
      },
      "websocket" : {
        "host" : "",
        "path" : "/",
        "listen" : { "ws" : 0, "wss" : 0 },
        "verify-peer" : true
      },
      "client" : {
        "guid" : "{F4569420-4E49-4CBA-9C36-94E722C8E363}",
        "server" : "ppp://192.168.0.24:20000/",
        "bandwidth" : 0,
        "reconnections" : { "timeout" : 5 },
        "paper-airplane" : { "tcp" : true },
        "http-proxy" : { "bind" : "127.0.0.1", "port" : 8080 },
        "socks-proxy" : { "bind" : "127.0.0.1", "port" : 1080 },
        "mappings" : []
      }
    }
    """
}

// MARK: - VPN Controller

final class VPNController {
    static let shared = VPNController()
    static let didChangeNotification = Notification.Name("OpenPPP2VPNControllerDidChange")

    private var manager: NETunnelProviderManager?
    private(set) var status: NEVPNStatus = .invalid
    private(set) var lastError: String?
#if targetEnvironment(simulator)
    private var simulatorPreviewConnected = false
#endif

    private init() {
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(statusDidChange),
            name: .NEVPNStatusDidChange,
            object: nil
        )
    }

    var isActive: Bool {
        status == .connected
    }

    var requiresRestartToApplyConfiguration: Bool {
        status == .connected || status == .connecting || status == .reasserting
    }

    func refresh(completion: (() -> Void)? = nil) {
#if targetEnvironment(simulator)
        status = simulatorPreviewConnected ? .connected : .disconnected
        emitChange()
        completion?()
#else
        loadManager { [weak self] manager, _ in
            self?.manager = manager
            self?.status = manager?.connection.status ?? .disconnected
            self?.emitChange()
            completion?()
        }
#endif
    }

    func connect(profile: ConfigProfile, completion: @escaping (Result<Void, Error>) -> Void) {
#if targetEnvironment(simulator)
        simulatorPreviewConnected = true
        lastError = nil
        status = .connected
        emitChange()
        completion(.success(()))
#else
        let options = profile.options
        let effectiveJson = ProfileStore.effectiveJson(profile.json, options: options)
        loadManager { [weak self] manager, error in
            if let error {
                self?.record(error)
                completion(.failure(error))
                return
            }

            let manager = manager ?? NETunnelProviderManager()
            let proto = (manager.protocolConfiguration as? NETunnelProviderProtocol) ?? NETunnelProviderProtocol()
            proto.providerBundleIdentifier = Self.providerBundleIdentifier
            proto.serverAddress = profile.serverEndpoint ?? profile.name
            proto.providerConfiguration = [
                "profileId": profile.id,
                "profileName": profile.name,
                "configJson": effectiveJson,
                "optionsJson": Self.encodeOptions(options)
            ]
            manager.localizedDescription = "OpenPPP2"
            manager.protocolConfiguration = proto
            manager.isEnabled = true

            manager.saveToPreferences { saveError in
                if let saveError {
                    self?.record(saveError)
                    completion(.failure(saveError))
                    return
                }

                manager.loadFromPreferences { loadError in
                    if let loadError {
                        self?.record(loadError)
                        completion(.failure(loadError))
                        return
                    }

                    do {
                        try manager.connection.startVPNTunnel()
                        self?.manager = manager
                        self?.lastError = nil
                        self?.status = manager.connection.status
                        self?.emitChange()
                        completion(.success(()))
                    } catch {
                        self?.record(error)
                        completion(.failure(error))
                    }
                }
            }
        }
#endif
    }

    func reconnect(profile: ConfigProfile, completion: @escaping (Result<Void, Error>) -> Void) {
#if targetEnvironment(simulator)
        connect(profile: profile, completion: completion)
#else
        guard requiresRestartToApplyConfiguration || status == .disconnecting else {
            connect(profile: profile, completion: completion)
            return
        }

        disconnect()
        waitForDisconnectThenConnect(profile: profile, attemptsRemaining: 12, completion: completion)
#endif
    }

    func disconnect() {
#if targetEnvironment(simulator)
        simulatorPreviewConnected = false
        status = .disconnected
        emitChange()
#else
        manager?.connection.stopVPNTunnel()
        status = manager?.connection.status ?? .disconnected
        emitChange()
#endif
    }

    func fetchStatistics(previous: VpnStatistics, completion: @escaping (VpnStatistics) -> Void) {
#if targetEnvironment(simulator)
        completion(simulatorPreviewConnected ? previous : .empty)
#else
        sendProviderMessage("stats") { data in
            guard let data,
                  let raw = String(data: data, encoding: .utf8),
                  !raw.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            else {
                completion(previous)
                return
            }
            completion(VpnStatistics(jsonText: raw, previous: previous))
        }
#endif
    }

    func fetchLinkState(completion: @escaping (Int) -> Void) {
#if targetEnvironment(simulator)
        completion(simulatorPreviewConnected ? 0 : 6)
#else
        sendProviderMessage("linkState") { data in
            guard let data,
                  let raw = String(data: data, encoding: .utf8),
                  let state = Int(raw.trimmingCharacters(in: .whitespacesAndNewlines))
            else {
                completion(6)
                return
            }
            completion(state)
        }
#endif
    }

    func fetchPacketTunnelCrashReports(completion: @escaping (CrashReporter.StoreSnapshot?) -> Void) {
#if targetEnvironment(simulator)
        completion(CrashReporter.StoreSnapshot(process: .packetTunnel, reportIDs: []))
#else
        sendProviderMessage("crashReports") { data in
            completion(CrashReporter.decodedStoreSnapshot(from: data))
        }
#endif
    }

    func deletePacketTunnelCrashReports(completion: (() -> Void)? = nil) {
#if targetEnvironment(simulator)
        completion?()
#else
        sendProviderMessage("deleteCrashReports") { _ in
            completion?()
        }
#endif
    }

    private func loadManager(completion: @escaping (NETunnelProviderManager?, Error?) -> Void) {
        NETunnelProviderManager.loadAllFromPreferences { managers, error in
            if let error {
                DispatchQueue.main.async { completion(nil, error) }
                return
            }

            let manager = managers?.first { existing in
                guard let proto = existing.protocolConfiguration as? NETunnelProviderProtocol else { return false }
                return proto.providerBundleIdentifier == Self.providerBundleIdentifier
            }
            DispatchQueue.main.async { completion(manager, nil) }
        }
    }

    private func sendProviderMessage(_ command: String, completion: @escaping (Data?) -> Void) {
        let send: (NETunnelProviderManager?) -> Void = { manager in
            guard let session = manager?.connection as? NETunnelProviderSession,
                  manager?.connection.status == .connected,
                  let data = command.data(using: .utf8)
            else {
                completion(nil)
                return
            }

            do {
                try session.sendProviderMessage(data) { response in
                    DispatchQueue.main.async {
                        completion(response)
                    }
                }
            } catch {
                self.record(error)
                completion(nil)
            }
        }

        if let manager {
            send(manager)
        } else {
            loadManager { [weak self] manager, _ in
                self?.manager = manager
                send(manager)
            }
        }
    }

    private func waitForDisconnectThenConnect(
        profile: ConfigProfile,
        attemptsRemaining: Int,
        completion: @escaping (Result<Void, Error>) -> Void
    ) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
            guard let self else { return }
            self.refresh {
                if self.status == .disconnected || self.status == .invalid || attemptsRemaining <= 0 {
                    self.connect(profile: profile, completion: completion)
                } else {
                    self.waitForDisconnectThenConnect(
                        profile: profile,
                        attemptsRemaining: attemptsRemaining - 1,
                        completion: completion
                    )
                }
            }
        }
    }

    @objc private func statusDidChange(_ notification: Notification) {
        guard let connection = notification.object as? NEVPNConnection else { return }
        status = connection.status
        emitChange()
    }

    private func record(_ error: Error) {
        lastError = error.localizedDescription
        emitChange()
    }

    private func emitChange() {
        NotificationCenter.default.post(name: Self.didChangeNotification, object: self)
    }

    private static var providerBundleIdentifier: String {
        "\(Bundle.main.bundleIdentifier ?? "io.github.miaocchi.openppp2").PacketTunnel"
    }

    private static func encodeOptions(_ options: LaunchOptions) -> String {
        guard let data = try? JSONEncoder().encode(options),
              let raw = String(data: data, encoding: .utf8)
        else { return "{}" }
        return raw
    }
}

// MARK: - Home

final class HomeViewController: UIViewController {
    private let store = ProfileStore.shared
    private let vpn = VPNController.shared
    private let scrollView = UIScrollView()
    private let content = UIStackView()
    private let powerButton = PowerButton()
    private let statusLabel = UILabel()
    private let subtitleLabel = UILabel()
    private let durationLabel = UILabel()
    private let profileButton = UIButton(type: .system)
    private let uploadCard = StatCard(title: "上行", symbol: "arrow.up", tint: .systemOrange)
    private let downloadCard = StatCard(title: "下行", symbol: "arrow.down", tint: .systemBlue)
    private let errorLabel = PaddingLabel()
    private var timer: Timer?
    private var pollTimer: Timer?
    private var connectedAt: Date?
    private var linkState = 6
    private var statistics = VpnStatistics.empty

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "OPENPPP2"
        view.backgroundColor = .systemGroupedBackground
        setupLayout()
        NotificationCenter.default.addObserver(self, selector: #selector(refreshUI), name: ProfileStore.didChangeNotification, object: nil)
        NotificationCenter.default.addObserver(self, selector: #selector(refreshUI), name: VPNController.didChangeNotification, object: nil)
        vpn.refresh()
        refreshUI()
    }

    deinit {
        timer?.invalidate()
        pollTimer?.invalidate()
        NotificationCenter.default.removeObserver(self)
    }

    private func setupLayout() {
        scrollView.translatesAutoresizingMaskIntoConstraints = false
        content.axis = .vertical
        content.spacing = 18
        content.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(scrollView)
        scrollView.addSubview(content)

        NSLayoutConstraint.activate([
            scrollView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            scrollView.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            scrollView.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            scrollView.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            content.leadingAnchor.constraint(equalTo: scrollView.contentLayoutGuide.leadingAnchor, constant: 20),
            content.trailingAnchor.constraint(equalTo: scrollView.contentLayoutGuide.trailingAnchor, constant: -20),
            content.topAnchor.constraint(equalTo: scrollView.contentLayoutGuide.topAnchor, constant: 16),
            content.bottomAnchor.constraint(equalTo: scrollView.contentLayoutGuide.bottomAnchor, constant: -24),
            content.widthAnchor.constraint(equalTo: scrollView.frameLayoutGuide.widthAnchor, constant: -40)
        ])

        let header = UILabel()
        header.text = "OPENPPP2"
        header.font = .systemFont(ofSize: 16, weight: .black)
        header.textAlignment = .center
        header.textColor = .label
        content.addArrangedSubview(header)

        powerButton.addTarget(self, action: #selector(toggleConnection), for: .touchUpInside)
        content.addArrangedSubview(powerButton)
        powerButton.heightAnchor.constraint(equalToConstant: 244).isActive = true

        statusLabel.textAlignment = .center
        statusLabel.font = .systemFont(ofSize: 24, weight: .bold)
        content.addArrangedSubview(statusLabel)

        subtitleLabel.textAlignment = .center
        subtitleLabel.font = .preferredFont(forTextStyle: .body)
        subtitleLabel.textColor = .secondaryLabel
        content.addArrangedSubview(subtitleLabel)

        durationLabel.textAlignment = .center
        durationLabel.font = .monospacedDigitSystemFont(ofSize: 15, weight: .medium)
        durationLabel.textColor = .secondaryLabel
        content.addArrangedSubview(durationLabel)

        let connectTitle = UILabel()
        connectTitle.text = "Connect to"
        connectTitle.font = .preferredFont(forTextStyle: .subheadline)
        connectTitle.textColor = .secondaryLabel
        content.addArrangedSubview(connectTitle)

        profileButton.backgroundColor = .secondarySystemGroupedBackground
        profileButton.layer.cornerRadius = 16
        profileButton.contentHorizontalAlignment = .left
        profileButton.titleLabel?.numberOfLines = 2
        profileButton.addTarget(self, action: #selector(openProfiles), for: .touchUpInside)
        content.addArrangedSubview(profileButton)
        profileButton.heightAnchor.constraint(greaterThanOrEqualToConstant: 76).isActive = true

        let stats = UIStackView(arrangedSubviews: [uploadCard, downloadCard])
        stats.axis = .horizontal
        stats.spacing = 12
        stats.distribution = .fillEqually
        content.addArrangedSubview(stats)

        errorLabel.backgroundColor = .systemRed.withAlphaComponent(0.12)
        errorLabel.textColor = .systemRed
        errorLabel.font = .preferredFont(forTextStyle: .footnote)
        errorLabel.numberOfLines = 0
        errorLabel.layer.cornerRadius = 12
        errorLabel.clipsToBounds = true
        content.addArrangedSubview(errorLabel)
    }

    @objc private func refreshUI() {
        let status = vpn.status
        let isOn = status == .connected
        let isBusy = status == .connecting || status == .disconnecting || status == .reasserting
        powerButton.apply(isOn: isOn, isBusy: isBusy)

        switch status {
        case .connected:
            if linkState == 0 {
                statusLabel.text = "Connected"
                subtitleLabel.text = "VPN is ON"
                connectedAt = connectedAt ?? Date()
                startTimer()
            } else {
                statusLabel.text = connectingText(for: linkState)
                subtitleLabel.text = "VPN is starting"
                startPolling()
            }
        case .connecting:
            statusLabel.text = connectingText(for: linkState)
            subtitleLabel.text = "VPN is starting"
            startPolling()
        case .disconnecting:
            statusLabel.text = "Disconnecting..."
            subtitleLabel.text = "VPN is OFF"
        case .reasserting:
            statusLabel.text = "Reconnecting..."
            subtitleLabel.text = "VPN is ON"
            startPolling()
        default:
            statusLabel.text = "Not Connected"
            subtitleLabel.text = "VPN is OFF"
            connectedAt = nil
            timer?.invalidate()
            pollTimer?.invalidate()
            pollTimer = nil
            durationLabel.text = "00:00:00"
            linkState = 6
            statistics = .empty
        }

        let profile = store.activeProfile()
        let name = profile?.name ?? "No profile"
        let endpoint = profile?.subtitle.isEmpty == false ? profile!.subtitle : (profile?.serverEndpoint ?? "点击选择一个配置")
        let title = "  \(profile?.flag.isEmpty == false ? profile!.flag : "◎")  \(name)\n  \(endpoint)"
        profileButton.setTitle(title, for: .normal)
        profileButton.setTitleColor(.label, for: .normal)

        uploadCard.set(value: "\(formatBytes(statistics.txSpeedBytes))/s", subtitle: "总 \(formatBytes(statistics.outBytes))")
        downloadCard.set(value: "\(formatBytes(statistics.rxSpeedBytes))/s", subtitle: "总 \(formatBytes(statistics.inBytes))")
        errorLabel.text = vpn.lastError
        errorLabel.isHidden = vpn.lastError == nil
    }

    private func startTimer() {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            guard let self, let connectedAt else { return }
            let duration = Int(Date().timeIntervalSince(connectedAt))
            let h = duration / 3600
            let m = (duration / 60) % 60
            let s = duration % 60
            self.durationLabel.text = String(format: "%02d:%02d:%02d", h, m, s)
        }
    }

    private func startPolling() {
        guard pollTimer == nil else { return }
        pollTimer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in
            self?.pollTunnel()
        }
        pollTunnel()
    }

    private func pollTunnel() {
        let status = vpn.status
        guard status == .connected || status == .connecting || status == .reasserting else {
            statistics = .empty
            linkState = 6
            pollTimer?.invalidate()
            pollTimer = nil
            refreshUI()
            return
        }

        vpn.fetchLinkState { [weak self] state in
            guard let self else { return }
            self.linkState = state
            if state == 0 {
                self.connectedAt = self.connectedAt ?? Date()
            }
            self.refreshUI()
        }

        vpn.fetchStatistics(previous: statistics) { [weak self] stats in
            guard let self else { return }
            self.statistics = stats
            self.refreshUI()
        }
    }

    @objc private func toggleConnection() {
        if vpn.status == .connected || vpn.status == .connecting || vpn.status == .reasserting {
            vpn.disconnect()
            return
        }

        guard let profile = store.activeProfile(), !profile.json.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
            openProfiles()
            return
        }

        powerButton.apply(isOn: false, isBusy: true)
        vpn.connect(profile: profile) { [weak self] result in
            guard let self else { return }
            if case let .failure(error) = result {
                self.presentError(error.localizedDescription)
            } else {
                self.startPolling()
            }
            self.refreshUI()
        }
    }

    @objc private func openProfiles() {
        let selector = SelectProfileViewController()
        let nav = UINavigationController(rootViewController: selector)
        present(nav, animated: true)
    }

    private func presentError(_ message: String) {
        let alert = UIAlertController(title: "连接失败", message: message, preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "关闭", style: .default))
        present(alert, animated: true)
    }

    private func connectingText(for state: Int) -> String {
        switch state {
        case 0: return "Connected"
        case 2: return "Initializing client..."
        case 3: return "Initializing exchanger..."
        case 4: return "Reconnecting..."
        case 5: return "Handshaking..."
        case 6: return "Starting engine..."
        default: return "Connecting..."
        }
    }

    private func formatBytes(_ bytes: Int) -> String {
        let value = Double(max(0, bytes))
        if value < 1024 { return "\(Int(value)) B" }
        if value < 1024 * 1024 { return String(format: "%.1f KB", value / 1024) }
        if value < 1024 * 1024 * 1024 { return String(format: "%.1f MB", value / (1024 * 1024)) }
        return String(format: "%.2f GB", value / (1024 * 1024 * 1024))
    }
}

// MARK: - Profile Selector

final class SelectProfileViewController: UITableViewController {
    private let store = ProfileStore.shared
    private let searchController = UISearchController(searchResultsController: nil)
    private var profiles: [ConfigProfile] = []
    private var activeId: String?
    private var query = ""

    private var filteredProfiles: [ConfigProfile] {
        let trimmed = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return profiles }
        return profiles.filter { profile in
            profile.name.localizedCaseInsensitiveContains(trimmed)
                || profile.subtitle.localizedCaseInsensitiveContains(trimmed)
                || (profile.serverEndpoint?.localizedCaseInsensitiveContains(trimmed) ?? false)
        }
    }

    private var favoriteProfiles: [ConfigProfile] {
        filteredProfiles.filter { $0.favorite }
    }

    private var otherProfiles: [ConfigProfile] {
        filteredProfiles.filter { !$0.favorite }
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "Select a Location"
        tableView.backgroundColor = .systemGroupedBackground
        tableView.register(ProfileCell.self, forCellReuseIdentifier: ProfileCell.reuseIdentifier)
        navigationItem.leftBarButtonItem = UIBarButtonItem(barButtonSystemItem: .close, target: self, action: #selector(close))
        navigationItem.rightBarButtonItem = UIBarButtonItem(barButtonSystemItem: .add, target: self, action: #selector(addProfile))

        searchController.obscuresBackgroundDuringPresentation = false
        searchController.searchResultsUpdater = self
        searchController.searchBar.placeholder = "搜索配置名称或地址..."
        navigationItem.searchController = searchController
        navigationItem.hidesSearchBarWhenScrolling = false
        definesPresentationContext = true

        NotificationCenter.default.addObserver(self, selector: #selector(reload), name: ProfileStore.didChangeNotification, object: nil)
        reload()
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    @objc private func reload() {
        profiles = store.profiles()
        activeId = store.activeProfile()?.id
        tableView.reloadData()
    }

    @objc private func close() {
        dismiss(animated: true)
    }

    @objc private func addProfile() {
        navigationController?.pushViewController(ProfileEditViewController(profile: nil), animated: true)
    }

    override func numberOfSections(in tableView: UITableView) -> Int {
        2
    }

    override func tableView(_ tableView: UITableView, titleForHeaderInSection section: Int) -> String? {
        section == 0 ? (favoriteProfiles.isEmpty ? nil : "Favorites") : "Locations"
    }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        section == 0 ? favoriteProfiles.count : otherProfiles.count
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: ProfileCell.reuseIdentifier, for: indexPath) as! ProfileCell
        let profile = profile(at: indexPath)
        cell.configure(profile: profile, isActive: profile.id == activeId, showsDisclosure: false)
        cell.accessoryType = profile.id == activeId ? .checkmark : .none
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        let profile = profile(at: indexPath)
        store.setActive(profile.id)
        promptRestartForActiveTunnelIfNeeded(
            profile: profile,
            message: "已切换到「\(profile.name)」。当前 VPN 正在运行，要让新配置生效需要重连。",
            noRestartMessage: nil,
            completion: { [weak self] in
                self?.dismiss(animated: true)
            }
        )
    }

    override func tableView(_ tableView: UITableView, trailingSwipeActionsConfigurationForRowAt indexPath: IndexPath) -> UISwipeActionsConfiguration? {
        let profile = profile(at: indexPath)
        let favorite = UIContextualAction(style: .normal, title: profile.favorite ? "取消收藏" : "收藏") { [weak self] _, _, done in
            self?.toggleFavorite(profile)
            done(true)
        }
        favorite.backgroundColor = .systemYellow

        let edit = UIContextualAction(style: .normal, title: "编辑") { [weak self] _, _, done in
            self?.navigationController?.pushViewController(ProfileEditViewController(profile: profile), animated: true)
            done(true)
        }
        edit.backgroundColor = .systemBlue
        return UISwipeActionsConfiguration(actions: [edit, favorite])
    }

    private func profile(at indexPath: IndexPath) -> ConfigProfile {
        indexPath.section == 0 ? favoriteProfiles[indexPath.row] : otherProfiles[indexPath.row]
    }

    private func toggleFavorite(_ profile: ConfigProfile) {
        var updated = profile
        updated.favorite.toggle()
        store.update(updated, snapshot: false)
    }
}

extension SelectProfileViewController: UISearchResultsUpdating {
    func updateSearchResults(for searchController: UISearchController) {
        query = searchController.searchBar.text ?? ""
        tableView.reloadData()
    }
}

// MARK: - Profiles

final class ProfilesViewController: UITableViewController {
    private let store = ProfileStore.shared
    private var profiles: [ConfigProfile] = []
    private var activeId: String?

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "配置文件"
        tableView.backgroundColor = .systemGroupedBackground
        tableView.register(ProfileCell.self, forCellReuseIdentifier: ProfileCell.reuseIdentifier)
        navigationItem.rightBarButtonItem = UIBarButtonItem(
            barButtonSystemItem: .add,
            target: self,
            action: #selector(addProfile)
        )
        NotificationCenter.default.addObserver(self, selector: #selector(reload), name: ProfileStore.didChangeNotification, object: nil)
        reload()
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    @objc private func reload() {
        profiles = store.profiles()
        activeId = store.activeProfile()?.id
        tableView.reloadData()
    }

    @objc private func addProfile() {
        let editor = ProfileEditViewController(profile: nil)
        navigationController?.pushViewController(editor, animated: true)
    }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        profiles.count
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: ProfileCell.reuseIdentifier, for: indexPath) as! ProfileCell
        let profile = profiles[indexPath.row]
        cell.configure(profile: profile, isActive: profile.id == activeId)
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        navigationController?.pushViewController(ProfileEditViewController(profile: profiles[indexPath.row]), animated: true)
    }

    override func tableView(_ tableView: UITableView, trailingSwipeActionsConfigurationForRowAt indexPath: IndexPath) -> UISwipeActionsConfiguration? {
        let profile = profiles[indexPath.row]
        let use = UIContextualAction(style: .normal, title: "当前") { [weak self] _, _, done in
            guard let self else {
                done(true)
                return
            }
            self.store.setActive(profile.id)
            self.promptRestartForActiveTunnelIfNeeded(
                profile: profile,
                message: "已切换到「\(profile.name)」。当前 VPN 正在运行，要让新配置生效需要重连。",
                noRestartMessage: nil
            )
            done(true)
        }
        use.backgroundColor = .systemBlue

        let delete = UIContextualAction(style: .destructive, title: "删除") { [weak self] _, _, done in
            self?.confirmDelete(profile)
            done(true)
        }

        return UISwipeActionsConfiguration(actions: profile.id == activeId ? [delete] : [delete, use])
    }

    private func confirmDelete(_ profile: ConfigProfile) {
        let alert = UIAlertController(title: "删除配置", message: "确定要删除「\(profile.name)」吗？", preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "取消", style: .cancel))
        alert.addAction(UIAlertAction(title: "删除", style: .destructive) { [weak self] _ in
            self?.store.remove(profile.id)
        })
        present(alert, animated: true)
    }
}

final class ProfileCell: UITableViewCell {
    static let reuseIdentifier = "ProfileCell"

    private let badge = UILabel()
    private let nameLabel = UILabel()
    private let subtitleLabel = UILabel()
    private let activeBadge = UILabel()

    override init(style: UITableViewCell.CellStyle, reuseIdentifier: String?) {
        super.init(style: style, reuseIdentifier: reuseIdentifier)
        accessoryType = .disclosureIndicator
        backgroundColor = .secondarySystemGroupedBackground

        badge.textAlignment = .center
        badge.font = .systemFont(ofSize: 20, weight: .semibold)
        badge.backgroundColor = .systemBlue.withAlphaComponent(0.10)
        badge.layer.cornerRadius = 18
        badge.clipsToBounds = true

        nameLabel.font = .systemFont(ofSize: 16, weight: .semibold)
        subtitleLabel.font = .preferredFont(forTextStyle: .footnote)
        subtitleLabel.textColor = .secondaryLabel
        subtitleLabel.numberOfLines = 1

        activeBadge.text = "ACTIVE"
        activeBadge.font = .systemFont(ofSize: 10, weight: .bold)
        activeBadge.textColor = .white
        activeBadge.backgroundColor = .systemBlue
        activeBadge.layer.cornerRadius = 7
        activeBadge.clipsToBounds = true
        activeBadge.textAlignment = .center

        let titleRow = UIStackView(arrangedSubviews: [nameLabel, activeBadge])
        titleRow.axis = .horizontal
        titleRow.spacing = 8
        titleRow.alignment = .center
        let textStack = UIStackView(arrangedSubviews: [titleRow, subtitleLabel])
        textStack.axis = .vertical
        textStack.spacing = 3
        let row = UIStackView(arrangedSubviews: [badge, textStack])
        row.axis = .horizontal
        row.spacing = 12
        row.alignment = .center
        row.translatesAutoresizingMaskIntoConstraints = false
        contentView.addSubview(row)

        NSLayoutConstraint.activate([
            badge.widthAnchor.constraint(equalToConstant: 36),
            badge.heightAnchor.constraint(equalToConstant: 36),
            activeBadge.widthAnchor.constraint(equalToConstant: 50),
            activeBadge.heightAnchor.constraint(equalToConstant: 18),
            row.leadingAnchor.constraint(equalTo: contentView.leadingAnchor, constant: 14),
            row.trailingAnchor.constraint(equalTo: contentView.trailingAnchor, constant: -8),
            row.topAnchor.constraint(equalTo: contentView.topAnchor, constant: 12),
            row.bottomAnchor.constraint(equalTo: contentView.bottomAnchor, constant: -12)
        ])
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func configure(profile: ConfigProfile, isActive: Bool, showsDisclosure: Bool = true) {
        accessoryType = showsDisclosure ? .disclosureIndicator : .none
        badge.text = profile.flag.isEmpty ? "◎" : profile.flag
        nameLabel.text = profile.name
        let sub = profile.subtitle.isEmpty ? (profile.serverEndpoint ?? "未配置服务器") : profile.subtitle
        subtitleLabel.text = sub
        activeBadge.isHidden = !isActive
    }
}

// MARK: - Profile Editor

final class ProfileEditViewController: UIViewController, UITextViewDelegate {
    private let store = ProfileStore.shared
    private let original: ConfigProfile?
    private let scrollView = UIScrollView()
    private let content = UIStackView()

    private let nameField = FormTextField(label: "名称")
    private let subtitleField = FormTextField(label: "副标题 / 城市")
    private let flagField = FormTextField(label: "图标 / Emoji")
    private let hostField = FormTextField(label: "Host")
    private let portField = FormTextField(label: "Port", keyboard: .numberPad)
    private let bandwidthField = FormTextField(label: "Bandwidth", keyboard: .numberPad)
    private let guidField = FormTextField(label: "GUID")
    private let protocolField = FormTextField(label: "Protocol")
    private let protocolKeyField = FormTextField(label: "Protocol Key")
    private let transportField = FormTextField(label: "Transport")
    private let transportKeyField = FormTextField(label: "Transport Key")
    private let httpPortField = FormTextField(label: "HTTP Port", keyboard: .numberPad)
    private let socksPortField = FormTextField(label: "SOCKS Port", keyboard: .numberPad)
    private let rawTextView = UITextView()

    private var jsonMap: [String: Any] = [:]

    init(profile: ConfigProfile?) {
        self.original = profile
        super.init(nibName: nil, bundle: nil)
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        title = original == nil ? "新增配置" : "编辑配置"
        view.backgroundColor = .systemGroupedBackground
        navigationItem.rightBarButtonItem = UIBarButtonItem(
            barButtonSystemItem: .save,
            target: self,
            action: #selector(save)
        )
        setupLayout()
        hydrate()
    }

    private func setupLayout() {
        scrollView.translatesAutoresizingMaskIntoConstraints = false
        content.axis = .vertical
        content.spacing = 12
        content.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(scrollView)
        scrollView.addSubview(content)

        NSLayoutConstraint.activate([
            scrollView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            scrollView.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            scrollView.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            scrollView.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            content.leadingAnchor.constraint(equalTo: scrollView.contentLayoutGuide.leadingAnchor, constant: 16),
            content.trailingAnchor.constraint(equalTo: scrollView.contentLayoutGuide.trailingAnchor, constant: -16),
            content.topAnchor.constraint(equalTo: scrollView.contentLayoutGuide.topAnchor, constant: 16),
            content.bottomAnchor.constraint(equalTo: scrollView.contentLayoutGuide.bottomAnchor, constant: -24),
            content.widthAnchor.constraint(equalTo: scrollView.frameLayoutGuide.widthAnchor, constant: -32)
        ])

        content.addArrangedSubview(SectionView(title: "基本信息", symbol: "bookmark", views: [nameField, subtitleField, flagField]))

        let serverRow = row([hostField, portField], weights: [3, 1])
        content.addArrangedSubview(SectionView(title: "服务器", symbol: "cloud", views: [serverRow, bandwidthField, guidField]))

        let cipherRow = row([protocolField, transportField], weights: [1, 1])
        content.addArrangedSubview(SectionView(title: "加密", symbol: "shield", views: [cipherRow, protocolKeyField, transportKeyField]))

        let proxyRow = row([httpPortField, socksPortField], weights: [1, 1])
        content.addArrangedSubview(SectionView(title: "本地代理", symbol: "network", views: [proxyRow]))

        rawTextView.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
        rawTextView.layer.borderColor = UIColor.separator.cgColor
        rawTextView.layer.borderWidth = 1
        rawTextView.layer.cornerRadius = 10
        rawTextView.delegate = self
        rawTextView.isScrollEnabled = true
        rawTextView.heightAnchor.constraint(equalToConstant: 280).isActive = true
        content.addArrangedSubview(SectionView(title: "高级 Raw JSON", symbol: "chevron.left.forwardslash.chevron.right", views: [rawTextView]))

        let saveButton = UIButton(type: .system)
        saveButton.setTitle("保存配置", for: .normal)
        saveButton.titleLabel?.font = .systemFont(ofSize: 17, weight: .semibold)
        saveButton.backgroundColor = .systemBlue
        saveButton.tintColor = .white
        saveButton.layer.cornerRadius = 12
        saveButton.addTarget(self, action: #selector(save), for: .touchUpInside)
        saveButton.heightAnchor.constraint(equalToConstant: 48).isActive = true
        content.addArrangedSubview(saveButton)
    }

    private func hydrate() {
        let profile = original ?? ProfileStore.defaultProfile()
        nameField.text = original?.name ?? "New Profile"
        subtitleField.text = original?.subtitle ?? ""
        flagField.text = original?.flag ?? ""
        rawTextView.text = prettify(profile.json)
        jsonMap = parseJson(rawTextView.text)
        hydrateFormFromJson()
    }

    private func hydrateFormFromJson() {
        let client = jsonMap["client"] as? [String: Any] ?? [:]
        let key = jsonMap["key"] as? [String: Any] ?? [:]
        if let server = client["server"] as? String, let url = URL(string: server) {
            hostField.text = url.host ?? ""
            portField.text = url.port.map(String.init) ?? ""
            if subtitleField.text?.isEmpty != false, let host = url.host {
                subtitleField.text = url.port == nil ? host : "\(host):\(url.port!)"
            }
        }
        bandwidthField.text = "\(client["bandwidth"] as? Int ?? 0)"
        guidField.text = client["guid"] as? String ?? ""
        protocolField.text = key["protocol"] as? String ?? "aes-128-cfb"
        protocolKeyField.text = key["protocol-key"] as? String ?? ""
        transportField.text = key["transport"] as? String ?? "aes-256-cfb"
        transportKeyField.text = key["transport-key"] as? String ?? ""
        let http = client["http-proxy"] as? [String: Any] ?? [:]
        let socks = client["socks-proxy"] as? [String: Any] ?? [:]
        httpPortField.text = "\(http["port"] as? Int ?? 8080)"
        socksPortField.text = "\(socks["port"] as? Int ?? 1080)"
    }

    @objc private func save() {
        applyFormToJson()
        guard let json = prettyJson(jsonMap) else {
            presentMessage(title: "Raw JSON 格式错误", message: "配置无法保存，请检查 JSON。")
            return
        }

        var profile = original ?? ProfileStore.defaultProfile()
        profile.name = nameField.textValue.isEmpty ? profile.name : nameField.textValue
        profile.subtitle = subtitleField.textValue
        profile.flag = flagField.textValue
        profile.json = json

        if original == nil {
            store.add(profile)
        } else {
            store.update(profile)
        }

        if store.activeProfile()?.id == profile.id {
            promptRestartForActiveTunnelIfNeeded(
                profile: profile,
                message: "当前 VPN 正在运行，保存后的配置需要重连才会生效。",
                noRestartMessage: nil,
                completion: { [weak self] in
                    self?.navigationController?.popViewController(animated: true)
                }
            )
        } else {
            navigationController?.popViewController(animated: true)
        }
    }

    func textViewDidEndEditing(_ textView: UITextView) {
        jsonMap = parseJson(textView.text)
        hydrateFormFromJson()
    }

    private func applyFormToJson() {
        if let rawData = rawTextView.text.data(using: .utf8),
           let object = try? JSONSerialization.jsonObject(with: rawData),
           let raw = object as? [String: Any] {
            jsonMap = raw
        }

        var client = jsonMap["client"] as? [String: Any] ?? [:]
        var key = jsonMap["key"] as? [String: Any] ?? [:]
        let host = hostField.textValue
        let port = Int(portField.textValue) ?? 0
        if !host.isEmpty {
            client["server"] = "ppp://\(host)\(port > 0 ? ":\(port)" : "")/"
        }
        client["guid"] = guidField.textValue
        client["bandwidth"] = Int(bandwidthField.textValue) ?? 0
        key["protocol"] = protocolField.textValue.isEmpty ? "aes-128-cfb" : protocolField.textValue
        key["protocol-key"] = protocolKeyField.textValue
        key["transport"] = transportField.textValue.isEmpty ? "aes-256-cfb" : transportField.textValue
        key["transport-key"] = transportKeyField.textValue
        var http = client["http-proxy"] as? [String: Any] ?? [:]
        var socks = client["socks-proxy"] as? [String: Any] ?? [:]
        http["bind"] = http["bind"] as? String ?? "127.0.0.1"
        socks["bind"] = socks["bind"] as? String ?? "127.0.0.1"
        http["port"] = Int(httpPortField.textValue) ?? 8080
        socks["port"] = Int(socksPortField.textValue) ?? 1080
        client["http-proxy"] = http
        client["socks-proxy"] = socks
        client["mappings"] = []
        jsonMap["client"] = client
        jsonMap["key"] = key
        rawTextView.text = prettyJson(jsonMap) ?? rawTextView.text
    }

    private func parseJson(_ text: String) -> [String: Any] {
        guard let data = text.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data),
              let map = object as? [String: Any]
        else { return [:] }
        return map
    }

    private func prettify(_ text: String) -> String {
        prettyJson(parseJson(text)) ?? text
    }

    private func prettyJson(_ map: [String: Any]) -> String? {
        guard JSONSerialization.isValidJSONObject(map),
              let data = try? JSONSerialization.data(withJSONObject: map, options: [.prettyPrinted, .sortedKeys])
        else { return nil }
        return String(data: data, encoding: .utf8)
    }
}

// MARK: - Options

final class OptionsViewController: UIViewController {
    private let store = ProfileStore.shared
    private let scrollView = UIScrollView()
    private let content = UIStackView()
    private var profile: ConfigProfile?
    private var options = LaunchOptions()

    private let tunIp = FormTextField(label: "TUN IP")
    private let tunMask = FormTextField(label: "TUN Mask")
    private let tunPrefix = FormTextField(label: "TUN Prefix", keyboard: .numberPad)
    private let gateway = FormTextField(label: "Gateway")
    private let mtu = FormTextField(label: "MTU", keyboard: .numberPad)
    private let route = FormTextField(label: "Route")
    private let routePrefix = FormTextField(label: "Route Prefix", keyboard: .numberPad)
    private let dns1 = FormTextField(label: "DNS 1")
    private let dns2 = FormTextField(label: "DNS 2")
    private let bypassList = FormTextView(label: "Bypass IP / CIDR，每行一条")
    private let dnsRules = FormTextView(label: "DNS Rules，每行一条")
    private let dnsDomestic = FormTextField(label: "Domestic DNS")
    private let dnsForeign = FormTextField(label: "Foreign DNS")
    private let stunCandidates = FormTextView(label: "STUN Candidates")
    private let geoCountry = FormTextField(label: "Geo Country")
    private let geoIpDat = FormTextField(label: "GeoIP.dat")
    private let geoSiteDat = FormTextField(label: "GeoSite.dat")
    private let mux = FormTextField(label: "Mux", keyboard: .numberPad)

    private let allowLan = UISwitch()
    private let blockQuic = UISwitch()
    private let vnet = UISwitch()
    private let staticMode = UISwitch()
    private let geoEnabled = UISwitch()
    private let ecsEnabled = UISwitch()
    private let tlsVerifyPeer = UISwitch()

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "启动参数"
        view.backgroundColor = .systemGroupedBackground
        navigationItem.rightBarButtonItems = [
            UIBarButtonItem(barButtonSystemItem: .save, target: self, action: #selector(save)),
            UIBarButtonItem(title: "默认", style: .plain, target: self, action: #selector(resetDefaults))
        ]
        setupLayout()
        NotificationCenter.default.addObserver(self, selector: #selector(load), name: ProfileStore.didChangeNotification, object: nil)
        load()
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    private func setupLayout() {
        scrollView.translatesAutoresizingMaskIntoConstraints = false
        content.axis = .vertical
        content.spacing = 12
        content.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(scrollView)
        scrollView.addSubview(content)

        NSLayoutConstraint.activate([
            scrollView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            scrollView.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            scrollView.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            scrollView.bottomAnchor.constraint(equalTo: view.bottomAnchor),
            content.leadingAnchor.constraint(equalTo: scrollView.contentLayoutGuide.leadingAnchor, constant: 16),
            content.trailingAnchor.constraint(equalTo: scrollView.contentLayoutGuide.trailingAnchor, constant: -16),
            content.topAnchor.constraint(equalTo: scrollView.contentLayoutGuide.topAnchor, constant: 16),
            content.bottomAnchor.constraint(equalTo: scrollView.contentLayoutGuide.bottomAnchor, constant: -24),
            content.widthAnchor.constraint(equalTo: scrollView.frameLayoutGuide.widthAnchor, constant: -32)
        ])

        content.addArrangedSubview(SectionView(title: "代理", symbol: "point.3.connected.trianglepath.dotted", views: [
            switchRow(title: "允许局域网代理", subtitle: "HTTP / SOCKS 监听 0.0.0.0", control: allowLan)
        ]))
        content.addArrangedSubview(SectionView(title: "DNS", symbol: "network", views: [
            row([dns1, dns2], weights: [1, 1]),
            dnsRules,
            row([dnsDomestic, dnsForeign], weights: [1, 1]),
            switchRow(title: "ECS", subtitle: "EDNS Client Subnet", control: ecsEnabled),
            switchRow(title: "TLS 校验", subtitle: "验证 DoH / DoT 上游证书", control: tlsVerifyPeer),
            stunCandidates
        ]))
        content.addArrangedSubview(SectionView(title: "Geo / Bypass", symbol: "globe.asia.australia", views: [
            switchRow(title: "启用 Geo 规则", subtitle: "生成 bypass 与 dns-rules", control: geoEnabled),
            geoCountry,
            row([geoIpDat, geoSiteDat], weights: [1, 1]),
            bypassList
        ]))
        content.addArrangedSubview(SectionView(title: "TUN 接口", symbol: "cable.connector", views: [
            row([tunIp, tunMask], weights: [1, 1]),
            row([tunPrefix, gateway], weights: [1, 1]),
            mtu
        ]))
        content.addArrangedSubview(SectionView(title: "路由", symbol: "arrow.triangle.branch", views: [
            row([route, routePrefix], weights: [2, 1])
        ]))
        content.addArrangedSubview(SectionView(title: "高级", symbol: "tuningfork", views: [
            mux,
            switchRow(title: "VNet", subtitle: "启用虚拟网卡路径", control: vnet),
            switchRow(title: "Block QUIC", subtitle: "屏蔽 UDP/443 防止绕过", control: blockQuic),
            switchRow(title: "Static Mode", subtitle: "UDP 静态隧道模式", control: staticMode)
        ]))

        let saveButton = UIButton(type: .system)
        saveButton.setTitle("保存启动参数", for: .normal)
        saveButton.titleLabel?.font = .systemFont(ofSize: 17, weight: .semibold)
        saveButton.backgroundColor = .systemBlue
        saveButton.tintColor = .white
        saveButton.layer.cornerRadius = 12
        saveButton.addTarget(self, action: #selector(save), for: .touchUpInside)
        saveButton.heightAnchor.constraint(equalToConstant: 48).isActive = true
        content.addArrangedSubview(saveButton)
    }

    @objc private func load() {
        profile = store.activeProfile()
        options = profile?.options ?? LaunchOptions()
        hydrate()
    }

    private func hydrate() {
        tunIp.text = options.tunIp
        tunMask.text = options.tunMask
        tunPrefix.text = String(options.tunPrefix)
        gateway.text = options.gateway
        mtu.text = String(options.mtu)
        route.text = options.route
        routePrefix.text = String(options.routePrefix)
        dns1.text = options.dns1
        dns2.text = options.dns2
        bypassList.text = options.bypassIpList
        dnsRules.text = options.dnsRulesList
        dnsDomestic.text = options.dnsDomestic
        dnsForeign.text = options.dnsForeign
        stunCandidates.text = options.dnsStunCandidates
        geoCountry.text = options.geoCountry
        geoIpDat.text = options.geoIpDat
        geoSiteDat.text = options.geoSiteDat
        mux.text = String(options.mux)
        allowLan.isOn = options.allowLan
        blockQuic.isOn = options.blockQuic
        vnet.isOn = options.vnet
        staticMode.isOn = options.staticMode
        geoEnabled.isOn = options.geoEnabled
        ecsEnabled.isOn = options.dnsEcsEnabled
        tlsVerifyPeer.isOn = options.dnsTlsVerifyPeer
    }

    @objc private func save() {
        guard var profile else {
            presentMessage(title: "没有配置文件", message: "请先在配置文件页创建并选择一个配置。")
            return
        }

        options.tunIp = tunIp.textValue
        options.tunMask = tunMask.textValue
        options.tunPrefix = Int(tunPrefix.textValue) ?? 24
        options.gateway = gateway.textValue
        options.mtu = Int(mtu.textValue) ?? 1400
        options.route = route.textValue
        options.routePrefix = Int(routePrefix.textValue) ?? 0
        options.dns1 = dns1.textValue
        options.dns2 = dns2.textValue
        options.bypassIpList = bypassList.textValue
        options.dnsRulesList = dnsRules.textValue
        options.dnsDomestic = dnsDomestic.textValue
        options.dnsForeign = dnsForeign.textValue
        options.dnsStunCandidates = stunCandidates.textValue
        options.geoCountry = geoCountry.textValue
        options.geoIpDat = geoIpDat.textValue
        options.geoSiteDat = geoSiteDat.textValue
        options.mux = Int(mux.textValue) ?? 0
        options.allowLan = allowLan.isOn
        options.blockQuic = blockQuic.isOn
        options.vnet = vnet.isOn
        options.staticMode = staticMode.isOn
        options.geoEnabled = geoEnabled.isOn
        options.dnsEcsEnabled = ecsEnabled.isOn
        options.dnsTlsVerifyPeer = tlsVerifyPeer.isOn
        profile.options = options
        store.update(profile, snapshot: false)
        promptRestartForActiveTunnelIfNeeded(
            profile: profile,
            message: "启动参数已保存。当前 VPN 正在运行，要让新参数生效需要重连。",
            noRestartMessage: "启动参数已保存"
        )
    }

    @objc private func resetDefaults() {
        options = LaunchOptions()
        hydrate()
    }
}

// MARK: - Settings

final class SettingsViewController: UITableViewController {
    private let store = ProfileStore.shared
    private let vpn = VPNController.shared
    private let rows = ["调试面板", "刷新 VPN 状态", "崩溃收集", "打开系统设置", "清空配置文件"]
    private var crashReportSummary: String?

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "设置"
        tableView.backgroundColor = .systemGroupedBackground
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: "Cell")
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        refreshCrashReportSummary()
    }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        rows.count
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "Cell", for: indexPath)
        var config = UIListContentConfiguration.valueCell()
        config.text = rows[indexPath.row]
        switch indexPath.row {
        case 0:
            config.secondaryText = store.debugPanelEnabled() ? "开启" : "关闭"
        case 1:
            config.secondaryText = statusText(vpn.status)
        case 2:
            config.secondaryText = crashReportSummary ?? CrashReporter.pendingReportsSummary()
        case 3:
            config.secondaryText = "iOS VPN / App 设置"
        default:
            config.secondaryText = "恢复 Default"
        }
        cell.contentConfiguration = config
        cell.accessoryType = indexPath.row == 0 ? .none : .disclosureIndicator
        if indexPath.row == 0 {
            let toggle = UISwitch()
            toggle.isOn = store.debugPanelEnabled()
            toggle.addAction(UIAction { [weak self] action in
                guard let sw = action.sender as? UISwitch else { return }
                self?.store.setDebugPanelEnabled(sw.isOn)
            }, for: .valueChanged)
            cell.accessoryView = toggle
        } else {
            cell.accessoryView = nil
        }
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        switch indexPath.row {
        case 1:
            vpn.refresh { [weak self] in self?.tableView.reloadData() }
        case 2:
            navigationController?.pushViewController(CrashReportsViewController(), animated: true)
        case 3:
            if let url = URL(string: UIApplication.openSettingsURLString) {
                UIApplication.shared.open(url)
            }
        case 4:
            confirmReset()
        default:
            break
        }
    }

    private func confirmReset() {
        let alert = UIAlertController(title: "清空配置文件", message: "这会删除所有本地配置并恢复 Default。", preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "取消", style: .cancel))
        alert.addAction(UIAlertAction(title: "清空", style: .destructive) { [weak self] _ in
            self?.store.resetAll()
            self?.tableView.reloadData()
        })
        present(alert, animated: true)
    }

    private func refreshCrashReportSummary() {
        let appSnapshots = CrashReporter.storeSnapshots
        crashReportSummary = CrashReporter.pendingReportsSummary(for: appSnapshots)
        guard !CrashReporter.isSharedContainerAvailable else {
            tableView.reloadData()
            return
        }

        vpn.fetchPacketTunnelCrashReports { [weak self] snapshot in
            guard let self else { return }
            var snapshots = appSnapshots
            if let snapshot {
                snapshots.removeAll { $0.process == snapshot.process }
                snapshots.append(snapshot)
                snapshots.sort { $0.process.rawValue < $1.process.rawValue }
            }
            self.crashReportSummary = CrashReporter.pendingReportsSummary(for: snapshots)
            self.tableView.reloadData()
        }
    }
}

final class CrashReportsViewController: UITableViewController {
    private let rows = ["状态", "存储位置", "清空本地报告"]
    private let vpn = VPNController.shared
    private var snapshots: [CrashReporter.StoreSnapshot] = []
    private var packetTunnelSnapshotAvailable = false

    override func viewDidLoad() {
        super.viewDidLoad()
        title = "崩溃收集"
        tableView.backgroundColor = .systemGroupedBackground
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: "Cell")
        reloadReports()
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        reloadReports()
    }

    private func reloadReports() {
        snapshots = CrashReporter.storeSnapshots
        packetTunnelSnapshotAvailable = CrashReporter.isSharedContainerAvailable
        tableView.reloadData()
        guard !CrashReporter.isSharedContainerAvailable else { return }

        vpn.fetchPacketTunnelCrashReports { [weak self] snapshot in
            guard let self else { return }
            if let snapshot {
                self.packetTunnelSnapshotAvailable = true
                self.upsert(snapshot)
                self.tableView.reloadData()
            } else {
                self.packetTunnelSnapshotAvailable = false
                self.removeSnapshot(for: .packetTunnel)
                self.tableView.reloadData()
            }
        }
    }

    override func numberOfSections(in tableView: UITableView) -> Int {
        let packetTunnelNeedsPlaceholder = !snapshots.contains { $0.process == .packetTunnel }
        return 1 + snapshots.count + (packetTunnelNeedsPlaceholder ? 1 : 0)
    }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        if section == 0 {
            return rows.count
        }
        guard let snapshot = snapshot(forSection: section) else {
            return 1
        }
        return max(snapshot.reportCount, 1)
    }

    override func tableView(_ tableView: UITableView, titleForHeaderInSection section: Int) -> String? {
        if section == 0 {
            return "KSCrash"
        }
        guard let snapshot = snapshot(forSection: section) else {
            return "\(CrashReporter.ProcessKind.packetTunnel.displayName) 待上传报告"
        }
        return "\(snapshot.process.displayName) 待上传报告"
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "Cell", for: indexPath)
        var config = UIListContentConfiguration.valueCell()

        if indexPath.section == 0 {
            config.text = rows[indexPath.row]
            switch indexPath.row {
            case 0:
                config.secondaryText = CrashReporter.pendingReportsSummary(for: snapshots)
                cell.accessoryType = .none
            case 1:
                config.secondaryText = CrashReporter.storageDescription()
                cell.accessoryType = .none
            default:
                config.secondaryText = "删除尚未上传的崩溃报告"
                cell.accessoryType = .disclosureIndicator
            }
        } else {
            guard let snapshot = snapshot(forSection: indexPath.section) else {
                config.text = "VPN 未连接"
                config.secondaryText = "连接后可读取扩展沙盒中的报告"
                cell.accessoryType = .none
                cell.contentConfiguration = config
                return cell
            }

            if snapshot.reportIDs.isEmpty {
                config.text = "没有待上传报告"
                config.secondaryText = nil
                cell.accessoryType = .none
            } else {
                let reportID = snapshot.reportIDs[indexPath.row]
                config.text = "Report \(reportID)"
                config.secondaryText = "等待 OpenTelemetry 上传"
                cell.accessoryType = .none
            }
        }

        cell.contentConfiguration = config
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        guard indexPath.section == 0, indexPath.row == 2 else { return }

        let alert = UIAlertController(title: "清空崩溃报告", message: "这会删除本机尚未上传的 KSCrash 报告。", preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "取消", style: .cancel))
        alert.addAction(UIAlertAction(title: "清空", style: .destructive) { [weak self] _ in
            CrashReporter.deleteAllReports()
            guard let self else { return }
            if CrashReporter.isSharedContainerAvailable {
                self.reloadReports()
            } else {
                self.vpn.deletePacketTunnelCrashReports {
                    self.reloadReports()
                }
            }
        })
        present(alert, animated: true)
    }

    private func snapshot(forSection section: Int) -> CrashReporter.StoreSnapshot? {
        let index = section - 1
        if snapshots.indices.contains(index) {
            return snapshots[index]
        }
        return nil
    }

    private func upsert(_ snapshot: CrashReporter.StoreSnapshot) {
        if let index = snapshots.firstIndex(where: { $0.process == snapshot.process }) {
            snapshots[index] = snapshot
        } else {
            snapshots.append(snapshot)
            snapshots.sort { $0.process.rawValue < $1.process.rawValue }
        }
    }

    private func removeSnapshot(for process: CrashReporter.ProcessKind) {
        snapshots.removeAll { $0.process == process }
    }
}

// MARK: - UI Helpers

final class PowerButton: UIControl {
    private let outer = UIView()
    private let middle = UIView()
    private let inner = UIView()
    private let symbol = UIImageView(image: UIImage(systemName: "power"))
    private let spinner = UIActivityIndicatorView(style: .large)

    override init(frame: CGRect) {
        super.init(frame: frame)
        setup()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    private func setup() {
        [outer, middle, inner, symbol, spinner].forEach { item in
            item.translatesAutoresizingMaskIntoConstraints = false
            item.isUserInteractionEnabled = false
        }
        addSubview(outer)
        addSubview(middle)
        addSubview(inner)
        addSubview(symbol)
        addSubview(spinner)
        symbol.tintColor = .white
        symbol.contentMode = .scaleAspectFit
        spinner.color = .white

        NSLayoutConstraint.activate([
            outer.centerXAnchor.constraint(equalTo: centerXAnchor),
            outer.centerYAnchor.constraint(equalTo: centerYAnchor),
            outer.widthAnchor.constraint(equalToConstant: 220),
            outer.heightAnchor.constraint(equalTo: outer.widthAnchor),
            middle.centerXAnchor.constraint(equalTo: centerXAnchor),
            middle.centerYAnchor.constraint(equalTo: centerYAnchor),
            middle.widthAnchor.constraint(equalToConstant: 170),
            middle.heightAnchor.constraint(equalTo: middle.widthAnchor),
            inner.centerXAnchor.constraint(equalTo: centerXAnchor),
            inner.centerYAnchor.constraint(equalTo: centerYAnchor),
            inner.widthAnchor.constraint(equalToConstant: 124),
            inner.heightAnchor.constraint(equalTo: inner.widthAnchor),
            symbol.centerXAnchor.constraint(equalTo: inner.centerXAnchor),
            symbol.centerYAnchor.constraint(equalTo: inner.centerYAnchor),
            symbol.widthAnchor.constraint(equalToConstant: 56),
            symbol.heightAnchor.constraint(equalToConstant: 56),
            spinner.centerXAnchor.constraint(equalTo: inner.centerXAnchor),
            spinner.centerYAnchor.constraint(equalTo: inner.centerYAnchor)
        ])
        apply(isOn: false, isBusy: false)
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        [outer, middle, inner].forEach { $0.layer.cornerRadius = $0.bounds.width / 2 }
        inner.layer.shadowRadius = 28
        inner.layer.shadowOpacity = 0.34
        inner.layer.shadowOffset = CGSize(width: 0, height: 10)
    }

    func apply(isOn: Bool, isBusy: Bool) {
        let color: UIColor = isOn ? .systemGreen : .systemRed
        outer.backgroundColor = color.withAlphaComponent(0.06)
        middle.backgroundColor = color.withAlphaComponent(0.12)
        inner.backgroundColor = color
        inner.layer.shadowColor = color.cgColor
        symbol.isHidden = isBusy
        isBusy ? spinner.startAnimating() : spinner.stopAnimating()
        accessibilityLabel = isOn ? "断开 VPN" : "连接 VPN"
    }
}

final class StatCard: UIView {
    private let valueLabel = UILabel()
    private let subtitleLabel = UILabel()

    init(title: String, symbol: String, tint: UIColor) {
        super.init(frame: .zero)
        backgroundColor = .secondarySystemGroupedBackground
        layer.cornerRadius = 12
        translatesAutoresizingMaskIntoConstraints = false

        let icon = UIImageView(image: UIImage(systemName: symbol))
        icon.tintColor = tint
        icon.contentMode = .scaleAspectFit
        icon.translatesAutoresizingMaskIntoConstraints = false
        icon.widthAnchor.constraint(equalToConstant: 22).isActive = true
        icon.heightAnchor.constraint(equalToConstant: 22).isActive = true

        let titleLabel = UILabel()
        titleLabel.text = title
        titleLabel.font = .preferredFont(forTextStyle: .footnote)
        titleLabel.textColor = .secondaryLabel

        valueLabel.font = .monospacedDigitSystemFont(ofSize: 17, weight: .bold)
        subtitleLabel.font = .preferredFont(forTextStyle: .caption1)
        subtitleLabel.textColor = .secondaryLabel
        let stack = UIStackView(arrangedSubviews: [icon, titleLabel, valueLabel, subtitleLabel])
        stack.axis = .vertical
        stack.alignment = .center
        stack.spacing = 5
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -12),
            stack.topAnchor.constraint(equalTo: topAnchor, constant: 14),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -14)
        ])
        set(value: "0 B/s", subtitle: "总 0 B")
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func set(value: String, subtitle: String) {
        valueLabel.text = value
        subtitleLabel.text = subtitle
    }
}

final class FormTextField: UIView {
    private let label = UILabel()
    let field = UITextField()

    var text: String? {
        get { field.text }
        set { field.text = newValue }
    }

    var textValue: String {
        (field.text ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
    }

    init(label title: String, keyboard: UIKeyboardType = .default) {
        super.init(frame: .zero)
        label.text = title
        label.font = .preferredFont(forTextStyle: .caption1)
        label.textColor = .secondaryLabel
        field.borderStyle = .roundedRect
        field.keyboardType = keyboard
        field.autocapitalizationType = .none
        field.autocorrectionType = .no
        let stack = UIStackView(arrangedSubviews: [label, field])
        stack.axis = .vertical
        stack.spacing = 4
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor),
            stack.topAnchor.constraint(equalTo: topAnchor),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor)
        ])
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
}

final class FormTextView: UIView {
    private let label = UILabel()
    let textView = UITextView()

    var text: String? {
        get { textView.text }
        set { textView.text = newValue }
    }

    var textValue: String {
        (textView.text ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
    }

    init(label title: String) {
        super.init(frame: .zero)
        label.text = title
        label.font = .preferredFont(forTextStyle: .caption1)
        label.textColor = .secondaryLabel
        textView.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
        textView.layer.borderWidth = 1
        textView.layer.borderColor = UIColor.separator.cgColor
        textView.layer.cornerRadius = 10
        textView.heightAnchor.constraint(equalToConstant: 90).isActive = true
        let stack = UIStackView(arrangedSubviews: [label, textView])
        stack.axis = .vertical
        stack.spacing = 4
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor),
            stack.topAnchor.constraint(equalTo: topAnchor),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor)
        ])
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
}

final class SectionView: UIView {
    init(title: String, symbol: String, views: [UIView]) {
        super.init(frame: .zero)
        backgroundColor = .secondarySystemGroupedBackground
        layer.cornerRadius = 14
        translatesAutoresizingMaskIntoConstraints = false

        let icon = UIImageView(image: UIImage(systemName: symbol))
        icon.tintColor = .systemBlue
        icon.contentMode = .scaleAspectFit
        icon.widthAnchor.constraint(equalToConstant: 18).isActive = true
        icon.heightAnchor.constraint(equalToConstant: 18).isActive = true

        let label = UILabel()
        label.text = title
        label.font = .systemFont(ofSize: 15, weight: .bold)
        let header = UIStackView(arrangedSubviews: [icon, label])
        header.axis = .horizontal
        header.spacing = 8
        header.alignment = .center

        let stack = UIStackView(arrangedSubviews: [header] + views)
        stack.axis = .vertical
        stack.spacing = 12
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 14),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -14),
            stack.topAnchor.constraint(equalTo: topAnchor, constant: 12),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -14)
        ])
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }
}

final class PaddingLabel: UILabel {
    var insets = UIEdgeInsets(top: 12, left: 12, bottom: 12, right: 12)

    override func drawText(in rect: CGRect) {
        super.drawText(in: rect.inset(by: insets))
    }

    override var intrinsicContentSize: CGSize {
        let size = super.intrinsicContentSize
        return CGSize(width: size.width + insets.left + insets.right, height: size.height + insets.top + insets.bottom)
    }
}

func row(_ views: [UIView], weights: [CGFloat]) -> UIStackView {
    let stack = UIStackView(arrangedSubviews: views)
    stack.axis = .horizontal
    stack.spacing = 8
    stack.distribution = .fillProportionally
    for (index, view) in views.enumerated() {
        view.setContentHuggingPriority(.defaultLow, for: .horizontal)
        view.widthAnchor.constraint(equalTo: stack.widthAnchor, multiplier: weights[index] / weights.reduce(0, +), constant: -8).priority = .defaultLow
    }
    return stack
}

func switchRow(title: String, subtitle: String, control: UISwitch) -> UIView {
    let titleLabel = UILabel()
    titleLabel.text = title
    titleLabel.font = .preferredFont(forTextStyle: .body)
    let subtitleLabel = UILabel()
    subtitleLabel.text = subtitle
    subtitleLabel.font = .preferredFont(forTextStyle: .caption1)
    subtitleLabel.textColor = .secondaryLabel
    subtitleLabel.numberOfLines = 2
    let labels = UIStackView(arrangedSubviews: [titleLabel, subtitleLabel])
    labels.axis = .vertical
    labels.spacing = 2
    let row = UIStackView(arrangedSubviews: [labels, control])
    row.axis = .horizontal
    row.spacing = 12
    row.alignment = .center
    labels.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
    return row
}

func statusText(_ status: NEVPNStatus) -> String {
    switch status {
    case .connected: return "已连接"
    case .connecting: return "连接中"
    case .disconnecting: return "断开中"
    case .reasserting: return "重连中"
    case .invalid: return "无配置"
    case .disconnected: return "未连接"
    @unknown default: return "未知"
    }
}

extension UIViewController {
    func presentMessage(title: String, message: String) {
        let alert = UIAlertController(title: title, message: message, preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "关闭", style: .default))
        present(alert, animated: true)
    }

    func presentToast(_ message: String) {
        let alert = UIAlertController(title: nil, message: message, preferredStyle: .alert)
        present(alert, animated: true)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) {
            alert.dismiss(animated: true)
        }
    }

    func promptRestartForActiveTunnelIfNeeded(
        profile: ConfigProfile,
        message: String,
        noRestartMessage: String?,
        completion: (() -> Void)? = nil
    ) {
        let vpn = VPNController.shared
        guard vpn.requiresRestartToApplyConfiguration else {
            if let noRestartMessage {
                presentToast(noRestartMessage)
            }
            completion?()
            return
        }

        let alert = UIAlertController(title: "重连以应用配置", message: message, preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: "稍后", style: .cancel) { _ in
            completion?()
        })
        alert.addAction(UIAlertAction(title: "立即重连", style: .default) { [weak self] _ in
            vpn.reconnect(profile: profile) { result in
                switch result {
                case .success:
                    self?.presentToast("已重连并应用配置")
                    completion?()
                case let .failure(error):
                    self?.presentMessage(title: "重连失败", message: error.localizedDescription)
                }
            }
        })
        present(alert, animated: true)
    }
}
