import UIKit

final class TelemetrySettingsViewController: UIViewController {
    private let store = TelemetrySettingsStore.shared
    private let profileStore = ProfileStore.shared
    private let vpn = VPNController.shared
    private let scrollView = UIScrollView()
    private let content = UIStackView()
    private let uploadSwitch = UISwitch()
    private let destinationControl = UISegmentedControl(items: TelemetrySettings.Destination.allCases.map(\.localizedDisplayName))
    private let endpointField = FormTextField(label: L10n.tr("telemetry.endpoint"), keyboard: .URL)
    private let crashSwitch = UISwitch()
    private let nativeSwitch = UISwitch()
    private let metricsSwitch = UISwitch()
    private let spansSwitch = UISwitch()
    private let levelControl = UISegmentedControl(items: ["telemetry.info", "telemetry.verbose", "telemetry.debug", "telemetry.trace"].map(L10n.tr))
    private let machineShortLabel = UILabel()
    private let machineFullLabel = UILabel()
    private let machineDeviceLabel = UILabel()
    private let developerEndpointLabel = UILabel()
    private let extensionNoteLabel = UILabel()
    private let uploadButton = UIButton(type: .system)
    private let uploadNote = UILabel()
    private var settings = TelemetrySettings()
    private var uploading = false

    override func viewDidLoad() {
        super.viewDidLoad()
        title = L10n.tr("settings.telemetry")
        view.backgroundColor = .systemGroupedBackground
        navigationItem.rightBarButtonItem = UIBarButtonItem(
            title: L10n.tr("common.save"), style: .done,
            target: self,
            action: #selector(save)
        )
        setupLayout()
        load()
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

        developerEndpointLabel.font = .preferredFont(forTextStyle: .footnote)
        developerEndpointLabel.adjustsFontForContentSizeCategory = true
        developerEndpointLabel.textColor = .secondaryLabel
        developerEndpointLabel.numberOfLines = 0

        extensionNoteLabel.font = .preferredFont(forTextStyle: .footnote)
        extensionNoteLabel.adjustsFontForContentSizeCategory = true
        extensionNoteLabel.textColor = .secondaryLabel
        extensionNoteLabel.numberOfLines = 0
        extensionNoteLabel.text = L10n.tr("telemetry.extensionNote")

        destinationControl.addTarget(self, action: #selector(destinationChanged), for: .valueChanged)
        destinationControl.accessibilityLabel = L10n.tr("telemetry.destination")
        levelControl.accessibilityLabel = L10n.tr("telemetry.logLevel")
        endpointField.field.addTarget(self, action: #selector(controlChanged), for: .editingChanged)
        [uploadSwitch, crashSwitch, nativeSwitch, metricsSwitch, spansSwitch].forEach {
            $0.addTarget(self, action: #selector(controlChanged), for: .valueChanged)
        }
        levelControl.addTarget(self, action: #selector(controlChanged), for: .valueChanged)

        [machineShortLabel, machineFullLabel, machineDeviceLabel].forEach {
            $0.font = UIFontMetrics(forTextStyle: .footnote).scaledFont(for: .monospacedSystemFont(ofSize: 13, weight: .regular))
            $0.adjustsFontForContentSizeCategory = true
            $0.textColor = .secondaryLabel
            $0.numberOfLines = 0
            $0.lineBreakMode = .byCharWrapping
        }

        let copyMachineButton = UIButton(type: .system)
        copyMachineButton.setImage(UIImage(systemName: "doc.on.doc"), for: .normal)
        copyMachineButton.setTitle(L10n.tr("telemetry.copyMachineId"), for: .normal)
        copyMachineButton.titleLabel?.font = .preferredFont(forTextStyle: .callout)
        copyMachineButton.titleLabel?.adjustsFontForContentSizeCategory = true
        copyMachineButton.heightAnchor.constraint(greaterThanOrEqualToConstant: 44).isActive = true
        copyMachineButton.addTarget(self, action: #selector(copyMachineId), for: .touchUpInside)
        copyMachineButton.contentHorizontalAlignment = .leading


        content.addArrangedSubview(SectionView(title: L10n.tr("telemetry.destination"), symbol: "arrow.up.doc", views: [
            switchRow(title: L10n.tr("telemetry.enableUpload"), subtitle: L10n.tr("telemetry.enableUpload.detail"), control: uploadSwitch),
            destinationControl,
            developerEndpointLabel,
            endpointField
        ]))

        content.addArrangedSubview(SectionView(title: L10n.tr("telemetry.data"), symbol: "checklist", views: [
            switchRow(title: "KSCrash", subtitle: L10n.tr("telemetry.crash.detail"), control: crashSwitch),
            switchRow(title: L10n.tr("telemetry.native"), subtitle: L10n.tr("telemetry.native.detail"), control: nativeSwitch)
        ]))

        content.addArrangedSubview(SectionView(title: L10n.tr("telemetry.logLevel"), symbol: "waveform.path.ecg", views: [
            levelControl,
            switchRow(title: L10n.tr("telemetry.metrics"), subtitle: L10n.tr("telemetry.metrics.detail"), control: metricsSwitch),
            switchRow(title: L10n.tr("telemetry.spans"), subtitle: L10n.tr("telemetry.spans.detail"), control: spansSwitch),
            extensionNoteLabel
        ]))
        content.addArrangedSubview(SectionView(title: L10n.tr("telemetry.identity"), symbol: "number", views: [
            machineShortLabel,
            machineFullLabel,
            machineDeviceLabel,
            copyMachineButton
        ]))

        uploadButton.setTitle(L10n.tr("telemetry.uploadCrashReports"), for: .normal)
        uploadButton.titleLabel?.font = .preferredFont(forTextStyle: .headline)
        uploadButton.titleLabel?.adjustsFontForContentSizeCategory = true
        uploadButton.titleLabel?.numberOfLines = 0
        uploadButton.backgroundColor = AppTheme.primary
        uploadButton.tintColor = .white
        uploadButton.layer.cornerRadius = 12
        uploadButton.addTarget(self, action: #selector(uploadCrashReports), for: .touchUpInside)
        uploadButton.heightAnchor.constraint(greaterThanOrEqualToConstant: 48).isActive = true
        uploadNote.font = .preferredFont(forTextStyle: .footnote)
        uploadNote.adjustsFontForContentSizeCategory = true
        uploadNote.textColor = .secondaryLabel
        uploadNote.numberOfLines = 0
        content.addArrangedSubview(SectionView(title: L10n.tr("settings.crashReports"), symbol: "doc", views: [uploadNote, uploadButton]))
    }

    private func load() {
        settings = store.settings()
        uploadSwitch.isOn = settings.uploadEnabled
        destinationControl.selectedSegmentIndex = TelemetrySettings.Destination.allCases.firstIndex(of: settings.destination) ?? 0
        endpointField.text = settings.customEndpoint
        crashSwitch.isOn = settings.includeCrashReports
        nativeSwitch.isOn = settings.includeNativeTelemetry
        metricsSwitch.isOn = false // Not available in the VPN extension.
        spansSwitch.isOn = settings.nativeSpansEnabled
        levelControl.selectedSegmentIndex = max(0, min(settings.nativeLogLevel, 3))
        refreshIdentity()
        refreshControls()
    }

    private func refreshIdentity() {
        let id = TelemetryIdentity.machineId
        machineShortLabel.text = L10n.format("telemetry.shortId", String(id.prefix(12)))
        machineFullLabel.text = "machine.id: \(id)"
        let attrs = TelemetryIdentity.nativeResourceAttributes
        let model = attrs["device.model"] ?? L10n.tr("vpn.unknown")
        let osName = attrs["os.name"] ?? "iOS"
        let osVersion = attrs["os.version"] ?? ""
        machineDeviceLabel.text = L10n.format("telemetry.device", model, osName, osVersion)
    }

    private func applyForm() -> TelemetrySettings {
        var next = settings
        next.uploadEnabled = uploadSwitch.isOn
        let destinations = TelemetrySettings.Destination.allCases
        let selected = max(0, min(destinationControl.selectedSegmentIndex, destinations.count - 1))
        next.destination = destinations[selected]
        next.customEndpoint = endpointField.textValue
        next.includeCrashReports = crashSwitch.isOn
        next.includeNativeTelemetry = nativeSwitch.isOn
        next.nativeSpansEnabled = spansSwitch.isOn
        next.nativeLogLevel = max(0, min(levelControl.selectedSegmentIndex, 3))
        return next
    }

    private func refreshControls() {
        let next = applyForm()
        let usesCustom = next.destination == .custom
        endpointField.isHidden = !usesCustom
        endpointField.field.isEnabled = usesCustom
        developerEndpointLabel.isHidden = usesCustom
        developerEndpointLabel.text = TelemetrySettings.developerEndpoint.isEmpty
            ? L10n.tr("telemetry.developerEndpointMissing")
            : TelemetrySettings.developerEndpoint
        let nativeControlsEnabled = next.canUpload && next.includeNativeTelemetry
        metricsSwitch.isEnabled = false
        spansSwitch.isEnabled = nativeControlsEnabled
        levelControl.isEnabled = nativeControlsEnabled
        metricsSwitch.alpha = 0.45
        spansSwitch.alpha = nativeControlsEnabled ? 1 : 0.45
        levelControl.alpha = nativeControlsEnabled ? 1 : 0.45
        uploadNote.text = L10n.tr(next.canUpload && next.includeCrashReports ? "crash.uploadDeletesLocal" : "telemetry.uploadSetup")
        uploadButton.isEnabled = next.canUpload && next.includeCrashReports && !uploading
        uploadButton.alpha = uploadButton.isEnabled ? 1 : 0.45
        uploadButton.setTitle(uploading ? L10n.tr("telemetry.uploading") : L10n.tr("telemetry.uploadCrashReports"), for: .normal)
    }

    @objc private func destinationChanged() {
        refreshControls()
    }

    @objc private func controlChanged() {
        refreshControls()
    }

    @objc private func copyMachineId() {
        confirmSensitiveAction(title: L10n.tr("telemetry.copyMachineId"), message: L10n.tr("telemetry.copyPrivacy"), action: L10n.tr("common.copy")) { [weak self] in
            UIPasteboard.general.setItems([["public.utf8-plain-text": TelemetryIdentity.machineId]], options: [.localOnly: true])
            self?.presentToast(L10n.tr("telemetry.machineCopied"))
        }
    }

    private func validatedForm() -> TelemetrySettings? {
        let next = applyForm()
        guard !next.uploadEnabled || next.destination != .custom || !next.customEndpoint.isEmpty else {
            presentMessage(title: L10n.tr("telemetry.endpointEmpty.title"), message: L10n.tr("telemetry.endpointEmpty.message"))
            return nil
        }

        if next.uploadEnabled && !next.effectiveEndpoint.isEmpty {
            let url = URLComponents(string: next.effectiveEndpoint)
            guard let scheme = url?.scheme?.lowercased(), ["http", "https"].contains(scheme), url?.host?.isEmpty == false else {
                presentMessage(title: L10n.tr("telemetry.endpointInvalid.title"), message: L10n.tr("telemetry.endpointInvalid.message"))
                return nil
            }
        }
        return next
    }

    @objc private func save() {
        guard let next = validatedForm() else { return }
        if next.canUpload && next != store.settings() {
            confirmSensitiveAction(title: L10n.tr("telemetry.saveConsent"), message: L10n.format("telemetry.saveConsent.detail", next.effectiveEndpoint), action: L10n.tr("common.save")) { [weak self] in
                self?.persist(next)
            }
        } else {
            persist(next)
        }
    }

    private func persist(_ next: TelemetrySettings) {
        settings = next

        store.save(settings)
        refreshControls()

        guard let profile = profileStore.activeProfile(), vpn.requiresRestartToApplyConfiguration else {
            presentToast(L10n.tr("telemetry.saved"))
            return
        }

        promptRestartForActiveTunnelIfNeeded(
            profile: profile,
            message: L10n.tr("telemetry.restartMessage"),
            noRestartMessage: L10n.tr("telemetry.saved")
        )
    }

    @objc private func uploadCrashReports() {
        guard let next = validatedForm() else { return }
        guard next.canUpload, next.includeCrashReports else {
            presentMessage(title: L10n.tr("telemetry.cannotUpload.title"), message: L10n.tr("telemetry.cannotUpload.message"))
            return
        }

        confirmReportUpload(to: next.effectiveEndpoint, savesSettings: true) { [weak self] in
            self?.sendReports(next)
        }
    }

    private func sendReports(_ next: TelemetrySettings) {
        settings = next
        store.save(settings)
        uploading = true
        refreshControls()
        var summary = TelemetryUploadSummary()
        let group = DispatchGroup()

        group.enter()
        CrashReporter.uploadReports(for: .app, settings: settings) { result in
            summary.merge(result)
            group.leave()
        }

        if CrashReporter.isSharedContainerAvailable {
            group.enter()
            CrashReporter.uploadReports(for: .packetTunnel, settings: settings) { result in
                summary.merge(result)
                group.leave()
            }
        } else {
            group.enter()
            vpn.uploadPacketTunnelCrashReports(settings: settings) { result in
                if let result {
                    summary.merge(result)
                } else {
                    summary.skipped += 1
                    summary.lastError = L10n.tr("telemetry.vpnUploadUnavailable")
                }
                group.leave()
            }
        }

        group.notify(queue: .main) { [weak self] in
            guard let self else { return }
            self.uploading = false
            self.refreshControls()
            let detail = summary.lastError.map { "\n\($0)" } ?? ""
            self.presentMessage(title: L10n.tr("crash.uploadFinished"), message: summary.localizedDisplayText + detail)
        }
    }
}
