import UIKit

// MARK: - Options

final class OptionsViewController: UIViewController {
    private let store = ProfileStore.shared
    private let scrollView = UIScrollView()
    private let content = UIStackView()
    private var options = LaunchOptions()
    private var lastLoadedOptions: LaunchOptions?
    private let scopeLabel = UILabel()

    private let tunIp = FormTextField(label: L10n.tr("options.tunAddress"))
    private let tunMask = FormTextField(label: L10n.tr("options.tunMask"))
    private let tunPrefix = FormTextField(label: L10n.tr("options.tunPrefix"), keyboard: .numberPad)
    private let gateway = FormTextField(label: L10n.tr("options.gateway"))
    private let mtu = FormTextField(label: "MTU", keyboard: .numberPad)
    private let dns1 = FormTextField(label: "DNS 1")
    private let dns2 = FormTextField(label: "DNS 2")
    private let bypassList = FormTextView(label: L10n.tr("options.bypass"))
    private let dnsRules = FormTextView(label: L10n.tr("options.dnsRules"))
    private let dnsDomestic = FormTextField(label: L10n.tr("options.domestic"))
    private let dnsForeign = FormTextField(label: L10n.tr("options.foreign"))
    private let dnsFakeIpRange = FormTextField(label: L10n.tr("options.fakeRange"))
    private let stunCandidates = FormTextView(label: L10n.tr("options.stun"))
    private let geoCountry = FormTextField(label: L10n.tr("options.country"))
    private let geoIpDat = FormTextField(label: "GeoIP.dat")
    private let geoSiteDat = FormTextField(label: "GeoSite.dat")
    private let mux = FormTextField(label: "VNet Mux", keyboard: .numberPad)
    private let httpProxyPort = FormTextField(label: L10n.tr("options.httpProxyPort"), keyboard: .numberPad)
    private let socksProxyPort = FormTextField(label: L10n.tr("options.socksProxyPort"), keyboard: .numberPad)

    private let allowLan = UISwitch()
    private let blockQuic = UISwitch()
    private let vnet = UISwitch()
    private let staticMode = UISwitch()
    private let autoReconnectOnPathRecovery = UISwitch()
    private let routeModeControl = UISegmentedControl(items: LaunchRouteMode.allCases.map(\.localizedDisplayName))
    private let ecsEnabled = UISwitch()
    private let fakeIpEnabled = UISwitch()
    private let tlsVerifyPeer = UISwitch()
    private var selectedMuxMode = "compat"

    override func viewDidLoad() {
        super.viewDidLoad()
        title = L10n.tr("options.title")
        view.backgroundColor = .systemGroupedBackground
        navigationItem.rightBarButtonItem = UIBarButtonItem(
            title: L10n.tr("options.save"),
            style: .done,
            target: self,
            action: #selector(save)
        )
        setupLayout()
        NotificationCenter.default.addObserver(self, selector: #selector(load), name: ProfileStore.didChangeNotification, object: nil)
        load()
    }

    deinit {
        NotificationCenter.default.removeObserver(self)
    }

    private func setupLayout() {
        scrollView.translatesAutoresizingMaskIntoConstraints = false
        scrollView.keyboardDismissMode = .interactive
        content.axis = .vertical
        content.spacing = 12
        content.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(scrollView)
        scrollView.addSubview(content)

        NSLayoutConstraint.activate([
            scrollView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            scrollView.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            scrollView.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            scrollView.bottomAnchor.constraint(equalTo: view.keyboardLayoutGuide.topAnchor),
            content.leadingAnchor.constraint(equalTo: scrollView.contentLayoutGuide.leadingAnchor, constant: 16),
            content.trailingAnchor.constraint(equalTo: scrollView.contentLayoutGuide.trailingAnchor, constant: -16),
            content.topAnchor.constraint(equalTo: scrollView.contentLayoutGuide.topAnchor, constant: 16),
            content.bottomAnchor.constraint(equalTo: scrollView.contentLayoutGuide.bottomAnchor, constant: -24),
            content.widthAnchor.constraint(equalTo: scrollView.frameLayoutGuide.widthAnchor, constant: -32)
        ])

        scopeLabel.font = .preferredFont(forTextStyle: .footnote)
        scopeLabel.text = L10n.tr("options.scope")
        scopeLabel.adjustsFontForContentSizeCategory = true
        scopeLabel.textColor = .secondaryLabel
        scopeLabel.numberOfLines = 0
        content.addArrangedSubview(scopeLabel)
        content.addArrangedSubview(SectionView(title: L10n.tr("options.connection"), symbol: "network", views: [
            routeModeRow(),
            switchRow(title: L10n.tr("options.autoReconnect"), subtitle: L10n.tr("options.autoReconnect.detail"), control: autoReconnectOnPathRecovery)
        ]))
        content.addArrangedSubview(SectionView(title: L10n.tr("options.proxy"), symbol: "point.3.connected.trianglepath.dotted", views: [
            httpProxyPort, socksProxyPort,
            switchRow(title: L10n.tr("options.allowLan"), subtitle: L10n.tr("options.allowLan.detail"), control: allowLan)
        ]))
        content.addArrangedSubview(SectionView(title: L10n.tr("options.dns"), symbol: "network", views: [
            dns1, dns2
        ]))
        let advancedRow = disclosureRow(
            title: L10n.tr("options.advancedParams"),
            subtitle: L10n.tr("options.advanced.detail")
        )
        advancedRow.addTarget(self, action: #selector(openAdvancedOptions), for: .touchUpInside)
        content.addArrangedSubview(SectionView(title: L10n.tr("options.advanced"), symbol: "tuningfork", views: [
            advancedRow
        ]))
        content.addArrangedSubview(resetDefaultsButton())
    }

    @objc private func load() {
        let saved = store.launchOptions()
        guard saved != lastLoadedOptions else { return }
        lastLoadedOptions = saved
        options = saved
        hydrate()
    }

    private func hydrate() {
        tunIp.text = options.tunIp
        tunMask.text = options.tunMask
        tunPrefix.text = String(options.tunPrefix)
        gateway.text = options.gateway
        mtu.text = String(options.mtu)
        dns1.text = options.dns1
        dns2.text = options.dns2
        bypassList.text = options.bypassIpList
        dnsRules.text = options.dnsRulesList
        dnsDomestic.text = options.dnsDomestic
        dnsForeign.text = options.dnsForeign
        dnsFakeIpRange.text = options.dnsFakeIpRange
        stunCandidates.text = options.dnsStunCandidates
        geoCountry.text = options.geoCountry
        geoIpDat.text = options.geoIpDat
        geoSiteDat.text = options.geoSiteDat
        mux.text = String(options.mux)
        selectedMuxMode = options.muxMode
        httpProxyPort.text = String(options.httpProxyPort)
        socksProxyPort.text = String(options.socksProxyPort)
        allowLan.isOn = options.allowLan
        blockQuic.isOn = options.blockQuic
        vnet.isOn = options.vnet
        staticMode.isOn = options.staticMode
        autoReconnectOnPathRecovery.isOn = options.autoReconnectOnPathRecovery
        routeModeControl.selectedSegmentIndex = LaunchRouteMode.allCases.firstIndex(of: options.routeMode) ?? 0
        ecsEnabled.isOn = options.dnsEcsEnabled
        fakeIpEnabled.isOn = options.dnsFakeIpEnabled
        tlsVerifyPeer.isOn = options.dnsTlsVerifyPeer
    }

    @objc private func save() {
        view.endEditing(true)
        guard let httpPort = Int(httpProxyPort.textValue), (1...65535).contains(httpPort),
              let socksPort = Int(socksProxyPort.textValue), (1...65535).contains(socksPort) else {
            presentMessage(title: L10n.tr("options.proxy"), message: L10n.tr("options.portError"))
            return
        }
        guard let prefix = Int(tunPrefix.textValue), (0...32).contains(prefix),
              let packetSize = Int(mtu.textValue), (1...65535).contains(packetSize),
              let muxValue = Int(mux.textValue), muxValue >= 0 else {
            presentMessage(title: L10n.tr("options.advancedParams"), message: L10n.tr("options.advancedError"))
            return
        }
        options.tunIp = tunIp.textValue
        options.tunMask = tunMask.textValue
        options.tunPrefix = prefix
        options.gateway = gateway.textValue
        options.mtu = packetSize
        options.dns1 = dns1.textValue
        options.dns2 = dns2.textValue
        options.bypassIpList = bypassList.textValue
        options.dnsRulesList = dnsRules.textValue
        options.dnsDomestic = dnsDomestic.textValue
        options.dnsForeign = dnsForeign.textValue
        options.dnsFakeIpRange = dnsFakeIpRange.textValue
        options.dnsStunCandidates = stunCandidates.textValue
        options.geoCountry = geoCountry.textValue
        options.geoIpDat = geoIpDat.textValue
        options.geoSiteDat = geoSiteDat.textValue
        options.mux = muxValue
        options.muxMode = selectedMuxMode
        options.httpProxyPort = httpPort
        options.socksProxyPort = socksPort
        options.allowLan = allowLan.isOn
        options.blockQuic = blockQuic.isOn
        options.vnet = vnet.isOn
        options.staticMode = staticMode.isOn
        options.autoReconnectOnPathRecovery = autoReconnectOnPathRecovery.isOn
        let routeModes = LaunchRouteMode.allCases
        options.setRouteMode(routeModes[max(0, min(routeModeControl.selectedSegmentIndex, routeModes.count - 1))])
        options.dnsEcsEnabled = ecsEnabled.isOn
        options.dnsFakeIpEnabled = fakeIpEnabled.isOn
        options.dnsTlsVerifyPeer = tlsVerifyPeer.isOn
        store.setLaunchOptions(options)

        if let profile = store.activeProfile() {
            promptRestartForActiveTunnelIfNeeded(
                profile: profile,
                message: L10n.tr("options.saved.message"),
                noRestartMessage: L10n.tr("options.saved.toast")
            )
        } else {
            presentToast(L10n.tr("options.saved.toast"))
        }
    }

    @objc private func resetDefaults() {
        options = LaunchOptions()
        hydrate()
    }

    @objc private func confirmResetDefaults() {
        let alert = UIAlertController(
            title: L10n.tr("options.reset.title"),
            message: L10n.tr("options.reset.message"),
            preferredStyle: .alert
        )
        alert.addAction(UIAlertAction(title: L10n.tr("common.cancel"), style: .cancel))
        alert.addAction(UIAlertAction(title: L10n.tr("options.reset.action"), style: .destructive) { [weak self] _ in
            self?.resetDefaults()
        })
        present(alert, animated: true)
    }

    private func resetDefaultsButton() -> UIButton {
        var configuration = UIButton.Configuration.tinted()
        configuration.title = L10n.tr("options.resetDefaults")
        configuration.baseForegroundColor = .systemRed
        configuration.baseBackgroundColor = .systemRed
        configuration.cornerStyle = .medium
        configuration.contentInsets = NSDirectionalEdgeInsets(top: 13, leading: 16, bottom: 13, trailing: 16)

        let button = UIButton(configuration: configuration, primaryAction: nil)
        button.addTarget(self, action: #selector(confirmResetDefaults), for: .touchUpInside)
        button.heightAnchor.constraint(greaterThanOrEqualToConstant: 48).isActive = true
        return button
    }

    private func routeModeRow() -> UIView {
        routeModeControl.selectedSegmentTintColor = AppTheme.primary
        routeModeControl.setTitleTextAttributes([.foregroundColor: UIColor.white], for: .selected)
        routeModeControl.setTitleTextAttributes([.foregroundColor: UIColor.label], for: .normal)
        routeModeControl.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

        let titleLabel = UILabel()
        titleLabel.text = L10n.tr("home.routeMode")
        titleLabel.font = .preferredFont(forTextStyle: .body)
        titleLabel.textColor = .label
        titleLabel.adjustsFontForContentSizeCategory = true
        titleLabel.numberOfLines = 0

        let detailLabel = UILabel()
        detailLabel.text = L10n.tr("options.routeMode.detail")
        detailLabel.font = .preferredFont(forTextStyle: .caption1)
        detailLabel.textColor = .secondaryLabel
        detailLabel.adjustsFontForContentSizeCategory = true
        detailLabel.numberOfLines = 0

        let labels = UIStackView(arrangedSubviews: [titleLabel, detailLabel])
        labels.axis = .vertical
        labels.spacing = 2
        labels.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

        let stack = UIStackView(arrangedSubviews: [labels, routeModeControl])
        routeModeControl.accessibilityLabel = titleLabel.text
        stack.axis = .vertical
        stack.spacing = 12
        return stack
    }

    @objc private func openAdvancedOptions() {
        let snapshot = TunnelSharedState.readRuntimeSnapshotJsonIfAlive()
            .flatMap { try? TunnelRuntimeBridge.decodeSnapshot($0) }
            ?? RuntimeSnapshot(
                generation: 0,
                monotonicMs: 0,
                phase: .idle,
                capabilities: RuntimeSnapshot.bundledCapabilities
            )
        let controller = OptionsAdvancedViewController(
            mux: mux,
            vnet: vnet,
            blockQuic: blockQuic,
            staticMode: staticMode,
            networkSections: [
                SectionView(title: L10n.tr("options.geoBypass"), symbol: "globe", views: [geoCountry, geoIpDat, geoSiteDat, bypassList]),
                SectionView(title: L10n.tr("options.dnsRules"), symbol: "network", views: [dnsRules, dnsDomestic, dnsForeign,
                    switchRow(title: "Fake-IP", subtitle: L10n.tr("options.fakeIp.detail"), control: fakeIpEnabled), dnsFakeIpRange,
                    switchRow(title: "ECS", subtitle: L10n.tr("options.ecs.detail"), control: ecsEnabled),
                    switchRow(title: L10n.tr("options.tlsVerify"), subtitle: L10n.tr("options.tlsVerify.detail"), control: tlsVerifyPeer), stunCandidates]),
                SectionView(title: L10n.tr("options.tun"), symbol: "cable.connector", views: [tunIp, tunMask, tunPrefix, gateway, mtu])
            ],
            muxModes: snapshot.availableMuxModes(experimental: store.debugPanelEnabled()),
            selectedMuxMode: selectedMuxMode,
            effectiveMuxMode: snapshot.effectiveMuxMode,
            onMuxModeChanged: { [weak self] in self?.selectedMuxMode = $0 }
        )
        navigationController?.pushViewController(controller, animated: true)
    }
}

final class OptionsAdvancedViewController: UIViewController {
    private let scrollView = UIScrollView()
    private let content = UIStackView()
    private let mux: FormTextField
    private let vnet: UISwitch
    private let blockQuic: UISwitch
    private let staticMode: UISwitch
    private let networkSections: [UIView]
    private let muxModes: [String]
    private let selectedMuxMode: String
    private let effectiveMuxMode: String
    private let onMuxModeChanged: (String) -> Void

    init(
        mux: FormTextField,
        vnet: UISwitch,
        blockQuic: UISwitch,
        staticMode: UISwitch,
        networkSections: [UIView],
        muxModes: [String],
        selectedMuxMode: String,
        effectiveMuxMode: String,
        onMuxModeChanged: @escaping (String) -> Void
    ) {
        self.mux = mux
        self.vnet = vnet
        self.blockQuic = blockQuic
        self.staticMode = staticMode
        self.networkSections = networkSections
        self.muxModes = muxModes
        self.selectedMuxMode = selectedMuxMode
        self.effectiveMuxMode = effectiveMuxMode
        self.onMuxModeChanged = onMuxModeChanged
        super.init(nibName: nil, bundle: nil)
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        title = L10n.tr("options.advancedParams")
        view.backgroundColor = .systemGroupedBackground
        setupLayout()
    }

    private func setupLayout() {
        scrollView.translatesAutoresizingMaskIntoConstraints = false
        scrollView.keyboardDismissMode = .interactive
        content.axis = .vertical
        content.spacing = 12
        content.translatesAutoresizingMaskIntoConstraints = false
        view.addSubview(scrollView)
        scrollView.addSubview(content)

        NSLayoutConstraint.activate([
            scrollView.leadingAnchor.constraint(equalTo: view.leadingAnchor),
            scrollView.trailingAnchor.constraint(equalTo: view.trailingAnchor),
            scrollView.topAnchor.constraint(equalTo: view.safeAreaLayoutGuide.topAnchor),
            scrollView.bottomAnchor.constraint(equalTo: view.keyboardLayoutGuide.topAnchor),
            content.leadingAnchor.constraint(equalTo: scrollView.contentLayoutGuide.leadingAnchor, constant: 16),
            content.trailingAnchor.constraint(equalTo: scrollView.contentLayoutGuide.trailingAnchor, constant: -16),
            content.topAnchor.constraint(equalTo: scrollView.contentLayoutGuide.topAnchor, constant: 16),
            content.bottomAnchor.constraint(equalTo: scrollView.contentLayoutGuide.bottomAnchor, constant: -24),
            content.widthAnchor.constraint(equalTo: scrollView.frameLayoutGuide.widthAnchor, constant: -32)
        ])

        let modeControl = UISegmentedControl(items: muxModes.map {
            $0 == "compat" ? L10n.tr("options.compat") : $0
        })
        modeControl.accessibilityLabel = L10n.tr("options.muxMode")
        modeControl.selectedSegmentIndex = muxModes.firstIndex(of: selectedMuxMode) ?? UISegmentedControl.noSegment
        modeControl.addAction(UIAction { [weak self, weak modeControl] _ in
            guard let self, let modeControl,
                  self.muxModes.indices.contains(modeControl.selectedSegmentIndex) else { return }
            self.onMuxModeChanged(self.muxModes[modeControl.selectedSegmentIndex])
        }, for: .valueChanged)
        let restartLabel = UILabel()
        restartLabel.text = L10n.tr("options.advancedSave")
        restartLabel.numberOfLines = 0
        restartLabel.adjustsFontForContentSizeCategory = true
        restartLabel.font = .preferredFont(forTextStyle: .footnote)
        restartLabel.textColor = .secondaryLabel
        let effectiveLabel = UILabel()
        effectiveLabel.font = .preferredFont(forTextStyle: .subheadline)
        effectiveLabel.numberOfLines = 0
        effectiveLabel.adjustsFontForContentSizeCategory = true
        let mode = effectiveMuxMode == "compat" ? L10n.tr("options.compat") : effectiveMuxMode
        effectiveLabel.text = mode.isEmpty ? L10n.tr("options.effectiveUnknown") : L10n.format("options.effective", mode)
        content.addArrangedSubview(restartLabel)
        networkSections.forEach(content.addArrangedSubview)

        content.addArrangedSubview(SectionView(title: "VNet", symbol: "network", views: [
            modeControl,
            effectiveLabel,
            mux,
            switchRow(title: "VNet", subtitle: L10n.tr("options.vnet.detail"), control: vnet)
        ]))
        content.addArrangedSubview(SectionView(title: L10n.tr("options.network"), symbol: "shield.lefthalf.filled", views: [
            switchRow(title: L10n.tr("home.blockQuic"), subtitle: L10n.tr("options.blockQuic.detail"), control: blockQuic),
            switchRow(title: L10n.tr("options.staticMode"), subtitle: L10n.tr("options.staticMode.detail"), control: staticMode)
        ]))
    }
}
