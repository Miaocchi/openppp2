import UIKit

// MARK: - Settings

final class SettingsViewController: UITableViewController {
    private let store = ProfileStore.shared
    private let vpn = VPNController.shared
    private enum SettingsItem {
        case language
        case debugSettings
        case refreshVPN
        case crashReports
        case telemetry
        case systemSettings
        case resetProfiles

        var title: String {
            switch self {
            case .language: return L10n.tr("language.title")
            case .debugSettings: return L10n.tr("settings.debug")
            case .refreshVPN: return L10n.tr("settings.refreshVPN")
            case .crashReports: return L10n.tr("settings.crashReports")
            case .telemetry: return L10n.tr("settings.telemetry")
            case .systemSettings: return L10n.tr("settings.systemSettings")
            case .resetProfiles: return L10n.tr("settings.resetProfiles")
            }
        }
    }

    private struct SettingsSection {
        var title: String
        var items: [SettingsItem]
    }

    private var sections: [SettingsSection] {
        [
            SettingsSection(title: L10n.tr("settings.section.app"), items: [.language, .systemSettings]),
            SettingsSection(title: L10n.tr("settings.section.connection"), items: [.refreshVPN, .debugSettings]),
            SettingsSection(title: L10n.tr("settings.section.privacy"), items: [.crashReports, .telemetry]),
            SettingsSection(title: L10n.tr("settings.section.storage"), items: [.resetProfiles])
        ]
    }
    private var crashReportSummary: String?

    override func viewDidLoad() {
        super.viewDidLoad()
        title = L10n.tr("tab.settings")
        tableView.backgroundColor = .systemGroupedBackground
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: "Cell")
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        tableView.reloadData()
        refreshCrashReportSummary()
    }

    override func numberOfSections(in tableView: UITableView) -> Int {
        sections.count
    }

    override func tableView(_ tableView: UITableView, titleForHeaderInSection section: Int) -> String? {
        sections[section].title
    }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        sections[section].items.count
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "Cell", for: indexPath)
        let item = sections[indexPath.section].items[indexPath.row]
        var config = UIListContentConfiguration.subtitleCell()
        config.secondaryTextProperties.numberOfLines = 0
        config.text = item.title
        switch item {
        case .language:
            config.secondaryText = AppLanguageStore.shared.language.displayName
        case .debugSettings:
            let debug = store.debugSettings()
            let enabled = store.debugPanelEnabled()
                || debug.packetFlowDiagnosticsEnabled
                || debug.packetFlowConsoleLoggingEnabled
                || debug.packetFlowTelemetryEnabled
            config.secondaryText = enabled ? L10n.tr("settings.debugEnabled") : L10n.tr("common.off")
        case .refreshVPN:
            config.secondaryText = vpn.statusRefreshError == nil ? statusText(vpn.status) : L10n.tr("vpn.unknown")
        case .crashReports:
            config.secondaryText = crashReportSummary ?? localizedCrashReportSummary(for: CrashReporter.storeSnapshots)
        case .telemetry:
            let telemetry = TelemetrySettingsStore.shared.settings()
            if !telemetry.uploadEnabled {
                config.secondaryText = L10n.tr("common.off")
            } else if telemetry.effectiveEndpoint.isEmpty {
                config.secondaryText = L10n.tr(telemetry.destination == .developer ? "telemetry.developerEndpointMissing" : "telemetry.endpointEmpty.title")
            } else {
                config.secondaryText = telemetry.destination.localizedDisplayName
            }
        case .systemSettings:
            config.secondaryText = L10n.tr("settings.systemSettings.detail")
        case .resetProfiles:
            config.secondaryText = L10n.tr("settings.resetProfiles.detail")
            config.textProperties.color = .systemRed
        }
        cell.contentConfiguration = config
        cell.accessoryType = item == .refreshVPN || item == .resetProfiles ? .none : .disclosureIndicator
        cell.accessoryView = nil
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        let item = sections[indexPath.section].items[indexPath.row]
        switch item {
        case .language:
            navigationController?.pushViewController(LanguageSettingsViewController(style: .insetGrouped), animated: true)
        case .debugSettings:
            navigationController?.pushViewController(DebugSettingsViewController(style: .insetGrouped), animated: true)
        case .refreshVPN:
            vpn.refresh { [weak self] in
                guard let self else { return }
                self.tableView.reloadData()
                UIAccessibility.post(notification: .announcement, argument: self.vpn.statusRefreshError == nil ? statusText(self.vpn.status) : L10n.tr("vpn.unknown"))
            }
        case .crashReports:
            navigationController?.pushViewController(CrashReportsViewController(style: .insetGrouped), animated: true)
        case .telemetry:
            navigationController?.pushViewController(TelemetrySettingsViewController(), animated: true)
        case .systemSettings:
            if let url = URL(string: UIApplication.openSettingsURLString) {
                UIApplication.shared.open(url)
            }
        case .resetProfiles:
            confirmReset()
        }
    }

    private func confirmReset() {
        let alert = UIAlertController(title: L10n.tr("settings.reset.title"), message: L10n.tr("settings.reset.message"), preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: L10n.tr("common.cancel"), style: .cancel))
        alert.addAction(UIAlertAction(title: L10n.tr("settings.reset.action"), style: .destructive) { [weak self] _ in
            self?.store.resetAll()
            self?.tableView.reloadData()
        })
        present(alert, animated: true)
    }

    private func refreshCrashReportSummary() {
        let appSnapshots = CrashReporter.storeSnapshots
        crashReportSummary = localizedCrashReportSummary(for: appSnapshots)
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
            self.crashReportSummary = localizedCrashReportSummary(for: snapshots)
            self.tableView.reloadData()
        }
    }
}

final class LanguageSettingsViewController: UITableViewController {
    private let languages = AppLanguage.allCases

    override func viewDidLoad() {
        super.viewDidLoad()
        title = L10n.tr("language.title")
        tableView.backgroundColor = .systemGroupedBackground
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: "Cell")
    }

    override func numberOfSections(in tableView: UITableView) -> Int {
        1
    }

    override func tableView(_ tableView: UITableView, titleForHeaderInSection section: Int) -> String? {
        L10n.tr("language.current")
    }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        languages.count
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let language = languages[indexPath.row]
        let cell = tableView.dequeueReusableCell(withIdentifier: "Cell", for: indexPath)
        var config = UIListContentConfiguration.subtitleCell()
        config.text = language.displayName
        config.secondaryText = language.detailText
        cell.contentConfiguration = config
        cell.accessoryType = language == AppLanguageStore.shared.language ? .checkmark : .none
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        AppLanguageStore.shared.language = languages[indexPath.row]
        tableView.reloadData()
    }
}
