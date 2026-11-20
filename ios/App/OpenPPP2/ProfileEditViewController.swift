import UIKit

// MARK: - Profile Editor

final class ProfileEditViewController: UIViewController, UITextViewDelegate {
    private let store = ProfileStore.shared
    private let original: ConfigProfile?
    private let documentPicker = ProfileDocumentPicker()
    private let scrollView = UIScrollView()
    private let content = UIStackView()

    private let nameField = FormTextField(label: L10n.tr("editor.name"))
    private let subtitleField = FormTextField(label: L10n.tr("editor.subtitle"))
    private let flagField = FormTextField(label: L10n.tr("editor.flag"))
    private let hostField = FormTextField(label: L10n.tr("editor.host"))
    private let portField = FormTextField(label: L10n.tr("editor.port"), keyboard: .numberPad)
    private let bandwidthField = FormTextField(label: L10n.tr("editor.bandwidth"), keyboard: .numberPad)
    private let guidField = FormTextField(label: "GUID")
    private let protocolField = FormTextField(label: L10n.tr("editor.protocol"))
    private let protocolKeyField = FormTextField(label: L10n.tr("editor.protocolKey"))
    private let transportField = FormTextField(label: L10n.tr("editor.transport"))
    private let transportKeyField = FormTextField(label: L10n.tr("editor.transportKey"))
    private let rawTextView = UITextView()

    private var jsonMap: [String: Any] = [:]
    private var formChanged = false

    init(profile: ConfigProfile?) {
        self.original = profile
        super.init(nibName: nil, bundle: nil)
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func viewDidLoad() {
        super.viewDidLoad()
        title = original == nil ? L10n.tr("editor.addTitle") : L10n.tr("editor.editTitle")
        view.backgroundColor = .systemGroupedBackground
        navigationItem.rightBarButtonItem = UIBarButtonItem(
            title: L10n.tr("common.save"), style: .done,
            target: self,
            action: #selector(save)
        )
        if original != nil {
            navigationItem.leftBarButtonItem = UIBarButtonItem(
                image: UIImage(systemName: "square.and.arrow.up"),
                style: .plain,
                target: self,
                action: #selector(exportProfile)
            )
            navigationItem.leftItemsSupplementBackButton = true
            navigationItem.leftBarButtonItem?.accessibilityLabel = L10n.tr("editor.export")
        }
        setupLayout()
        hydrate()
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

        content.addArrangedSubview(SectionView(title: L10n.tr("editor.basic"), symbol: "bookmark", views: [nameField, subtitleField, flagField]))

        content.addArrangedSubview(SectionView(title: L10n.tr("editor.server"), symbol: "cloud", views: [hostField, portField, guidField]))
        content.addArrangedSubview(SectionView(title: L10n.tr("editor.crypto"), symbol: "shield", views: [protocolField, protocolKeyField, transportField, transportKeyField]))
        for input in [hostField, portField, guidField, bandwidthField, protocolField, protocolKeyField, transportField, transportKeyField] {
            input.field.addTarget(self, action: #selector(markFormChanged), for: .editingChanged)
        }

        rawTextView.font = UIFontMetrics(forTextStyle: .footnote).scaledFont(for: .monospacedSystemFont(ofSize: 13, weight: .regular))
        rawTextView.adjustsFontForContentSizeCategory = true
        rawTextView.accessibilityLabel = L10n.tr("editor.rawJson")
        rawTextView.autocorrectionType = .no
        rawTextView.smartQuotesType = .no
        rawTextView.smartDashesType = .no
        rawTextView.layer.borderColor = UIColor.separator.cgColor
        rawTextView.layer.borderWidth = 1
        rawTextView.layer.cornerRadius = 10
        rawTextView.delegate = self
        rawTextView.isScrollEnabled = true
        rawTextView.heightAnchor.constraint(equalToConstant: 280).isActive = true
        let rawLabel = UILabel()
        rawLabel.text = rawTextView.accessibilityLabel
        rawLabel.font = .preferredFont(forTextStyle: .caption1)
        rawLabel.adjustsFontForContentSizeCategory = true
        rawLabel.isAccessibilityElement = false
        rawLabel.numberOfLines = 0
        let advanced = SectionView(title: L10n.tr("options.advanced"), symbol: "slider.horizontal.3", views: [bandwidthField, rawLabel, rawTextView])
        advanced.isHidden = true
        let disclosure = UIButton(configuration: .tinted())
        disclosure.setTitle(L10n.tr("editor.showAdvanced"), for: .normal)
        disclosure.addAction(UIAction { [weak self, weak disclosure] _ in
            guard let self else { return }
            self.view.endEditing(true)
            if !advanced.isHidden && self.parseJson(self.rawTextView.text) == nil {
                self.presentMessage(title: L10n.tr("editor.rawJsonError"), message: L10n.tr("editor.rawJsonError.message"))
                return
            }
            if advanced.isHidden && self.formChanged && self.parseJson(self.rawTextView.text) != nil && self.validateFields() {
                self.applyFormToJson()
                self.formChanged = false
            }
            advanced.isHidden.toggle()
            disclosure?.setTitle(L10n.tr(advanced.isHidden ? "editor.showAdvanced" : "editor.hideAdvanced"), for: .normal)
        }, for: .touchUpInside)
        content.addArrangedSubview(disclosure)
        content.addArrangedSubview(advanced)

        let saveButton = UIButton(type: .system)
        saveButton.setTitle(L10n.tr("editor.saveProfile"), for: .normal)
        saveButton.titleLabel?.font = .preferredFont(forTextStyle: .headline)
        saveButton.titleLabel?.adjustsFontForContentSizeCategory = true
        saveButton.backgroundColor = AppTheme.primary
        saveButton.tintColor = .white
        saveButton.layer.cornerRadius = 12
        saveButton.addTarget(self, action: #selector(save), for: .touchUpInside)
        saveButton.heightAnchor.constraint(greaterThanOrEqualToConstant: 48).isActive = true
        content.addArrangedSubview(saveButton)
    }

    private func hydrate() {
        let profile = original ?? ProfileStore.defaultProfile()
        nameField.text = original?.name ?? L10n.tr("profiles.addMenu.new")
        subtitleField.text = original?.subtitle ?? ""
        flagField.text = original?.flag ?? ""
        let parsed = parseJson(profile.json)
        jsonMap = parsed ?? [:]
        rawTextView.text = parsed.flatMap(prettyJson) ?? profile.json
        hydrateFormFromJson()
    }

    private func hydrateFormFromJson() {
        let client = jsonMap["client"] as? [String: Any] ?? [:]
        let key = jsonMap["key"] as? [String: Any] ?? [:]
        if let server = client["server"] as? String, let endpoint = PppServerEndpoint.parse(server) {
            hostField.text = endpoint.host
            portField.text = endpoint.port.map(String.init) ?? ""
            if subtitleField.text?.isEmpty != false {
                subtitleField.text = endpoint.port == nil ? endpoint.host : "\(endpoint.host):\(endpoint.port!)"
            }
        } else {
            hostField.text = ""
            portField.text = ""
        }
        bandwidthField.text = "\(client["bandwidth"] as? Int ?? 0)"
        guidField.text = client["guid"] as? String ?? ""
        protocolField.text = key["protocol"] as? String ?? "aes-128-cfb"
        protocolKeyField.text = key["protocol-key"] as? String ?? ""
        transportField.text = key["transport"] as? String ?? "aes-256-cfb"
        transportKeyField.text = key["transport-key"] as? String ?? ""
    }

    @objc private func exportProfile() {
        guard let profileId = original?.id else {
            presentMessage(title: L10n.tr("editor.exportUnavailable"), message: L10n.tr("editor.saveBeforeExport"))
            return
        }

        confirmProfileSecretsWarning(title: L10n.tr("editor.export")) { [weak self] in
            guard let self else { return }
            do {
                let bundle = try self.store.exportProfile(id: profileId)
                let data = try ProfileImportExportCodec.encode(bundle)
                let filename = ProfileImportExportCodec.singleProfileFilename(name: bundle.profiles[0].name)
                self.documentPicker.presentExport(data: data, filename: filename, from: self) { event in
                    switch event {
                    case .exportFinished:
                        self.presentToast(L10n.tr("editor.exported"))
                    case let .failed(error):
                        self.presentMessage(title: L10n.tr("editor.exportFailed"), message: error.localizedDescription)
                    case .cancelled, .imported:
                        break
                    }
                }
            } catch {
                self.presentMessage(title: L10n.tr("editor.exportFailed"), message: error.localizedDescription)
            }
        }
    }

    @objc private func save() {
        view.endEditing(true)
        guard !nameField.textValue.isEmpty else {
            presentMessage(title: L10n.tr("editor.name"), message: L10n.tr("editor.nameError"))
            return
        }
        guard let raw = parseJson(rawTextView.text) else {
            presentMessage(title: L10n.tr("editor.rawJsonError"), message: L10n.tr("editor.rawJsonError.message"))
            return
        }
        jsonMap = raw
        guard validateFields() else { return }
        if formChanged { applyFormToJson() }
        guard let json = prettyJson(jsonMap) else {
            presentMessage(title: L10n.tr("editor.rawJsonError"), message: L10n.tr("editor.rawJsonError.message"))
            return
        }

        var profile = original ?? ProfileStore.defaultProfile()
        profile.name = nameField.textValue
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
                message: L10n.tr("editor.restartAfterSave"),
                noRestartMessage: nil,
                completion: { [weak self] in
                    self?.navigationController?.popViewController(animated: true)
                }
            )
        } else {
            navigationController?.popViewController(animated: true)
        }
    }

    @objc private func markFormChanged() { formChanged = true }

    private func validateFields() -> Bool {
        let port = portField.textValue
        if !port.isEmpty && !(Int(port).map { (1...65535).contains($0) } ?? false) {
            presentMessage(title: L10n.tr("editor.port"), message: L10n.tr("editor.portError"))
            return false
        }
        if !bandwidthField.textValue.isEmpty && (Int(bandwidthField.textValue) ?? -1) < 0 {
            presentMessage(title: L10n.tr("editor.bandwidth"), message: L10n.tr("editor.numberError"))
            return false
        }
        return true
    }

    func textViewShouldBeginEditing(_ textView: UITextView) -> Bool {
        if formChanged, parseJson(textView.text) != nil {
            guard validateFields() else { return false }
            applyFormToJson()
            formChanged = false
        }
        return true
    }

    func textViewDidEndEditing(_ textView: UITextView) {
        guard let raw = parseJson(textView.text) else { return }
        jsonMap = raw
        hydrateFormFromJson()
        formChanged = false
    }

    private func applyFormToJson() {
        var client = jsonMap["client"] as? [String: Any] ?? [:]
        var key = jsonMap["key"] as? [String: Any] ?? [:]
        let currentServer = client["server"] as? String ?? ""
        let host = hostField.textValue
        let port = Int(portField.textValue) ?? 0
        if !host.isEmpty {
            client["server"] = serverString(
                preserving: currentServer,
                host: host,
                port: port
            )
        } else {
            client["server"] = ""
        }
        client["guid"] = guidField.textValue
        client["bandwidth"] = Int(bandwidthField.textValue) ?? 0
        key["protocol"] = protocolField.textValue.isEmpty ? "aes-128-cfb" : protocolField.textValue
        key["protocol-key"] = protocolKeyField.textValue
        key["transport"] = transportField.textValue.isEmpty ? "aes-256-cfb" : transportField.textValue
        key["transport-key"] = transportKeyField.textValue
        jsonMap["client"] = client
        jsonMap["key"] = key
        rawTextView.text = prettyJson(jsonMap) ?? rawTextView.text
    }

    private func serverString(preserving currentServer: String, host: String, port: Int) -> String {
        if let endpoint = PppServerEndpoint.parse(currentServer),
           endpoint.host == host,
           (endpoint.port ?? 0) == port {
            return currentServer
        }
        let authority = host.contains(":") && !host.hasPrefix("[") ? "[\(host)]" : host
        return "ppp://\(authority)\(port > 0 ? ":\(port)" : "")/"
    }

    private func parseJson(_ text: String) -> [String: Any]? {
        guard let data = text.data(using: .utf8),
              let object = try? JSONSerialization.jsonObject(with: data),
              let map = object as? [String: Any]
        else { return nil }
        return map
    }

    private func prettyJson(_ map: [String: Any]) -> String? {
        guard JSONSerialization.isValidJSONObject(map),
              let data = try? JSONSerialization.data(withJSONObject: map, options: [.prettyPrinted, .sortedKeys])
        else { return nil }
        return String(data: data, encoding: .utf8)
    }
}
