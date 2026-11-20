import UIKit

final class DebugSettingsViewController: UITableViewController {
    private let store = ProfileStore.shared

    private enum Item: Int {
        case debugPanel
        case packetDiagnostics
        case packetConsoleLogging
        case packetTelemetry

        var title: String {
            switch self {
            case .debugPanel: return L10n.tr("debug.item.panel")
            case .packetDiagnostics: return L10n.tr("debug.item.packetDiagnostics")
            case .packetConsoleLogging: return L10n.tr("debug.item.consoleLogging")
            case .packetTelemetry: return L10n.tr("debug.item.packetTelemetry")
            }
        }

        var subtitle: String {
            switch self {
            case .debugPanel:
                return L10n.tr("debug.item.panel.detail")
            case .packetDiagnostics:
                return L10n.tr("debug.item.packetDiagnostics.detail")
            case .packetConsoleLogging:
                return L10n.tr("debug.item.consoleLogging.detail")
            case .packetTelemetry:
                return L10n.tr("debug.item.packetTelemetry.detail")
            }
        }
    }

    private let sections: [[Item]] = [[.debugPanel], [.packetDiagnostics, .packetConsoleLogging], [.packetTelemetry]]

    override func viewDidLoad() {
        super.viewDidLoad()
        title = L10n.tr("debug.title")
        tableView.backgroundColor = .systemGroupedBackground
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: "Cell")
    }

    override func numberOfSections(in tableView: UITableView) -> Int {
        sections.count
    }

    override func tableView(_ tableView: UITableView, titleForHeaderInSection section: Int) -> String? {
        L10n.tr(["debug.section.ui", "debug.section.local", "telemetry.upload"][section])
    }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        sections[section].count
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "Cell", for: indexPath)
        let item = item(at: indexPath)
        var config = UIListContentConfiguration.subtitleCell()
        config.text = item.title
        config.secondaryText = item.subtitle
        config.secondaryTextProperties.numberOfLines = 0
        cell.contentConfiguration = config
        cell.selectionStyle = .none

        let toggle = UISwitch()
        toggle.isOn = isEnabled(item)
        toggle.accessibilityLabel = item.title
        toggle.accessibilityHint = item.subtitle
        toggle.tag = item.rawValue
        toggle.addTarget(self, action: #selector(toggleChanged(_:)), for: .valueChanged)
        cell.accessoryView = toggle
        return cell
    }

    override func tableView(_ tableView: UITableView, titleForFooterInSection section: Int) -> String? {
        section == 0 ? nil : L10n.tr("debug.packetTunnel.footer")
    }

    private func item(at indexPath: IndexPath) -> Item {
        sections[indexPath.section][indexPath.row]
    }

    private func isEnabled(_ item: Item) -> Bool {
        let debug = store.debugSettings()
        switch item {
        case .debugPanel:
            return store.debugPanelEnabled()
        case .packetDiagnostics:
            return debug.packetFlowDiagnosticsEnabled
        case .packetConsoleLogging:
            return debug.packetFlowConsoleLoggingEnabled
        case .packetTelemetry:
            return debug.packetFlowTelemetryEnabled
        }
    }

    @objc private func toggleChanged(_ sender: UISwitch) {
        guard let item = Item(rawValue: sender.tag) else { return }
        let enabled = sender.isOn
        if enabled && item != .debugPanel {
            sender.setOn(false, animated: true)
            confirmSensitiveAction(title: item.title, message: item.subtitle, action: L10n.tr("common.enable")) { [weak self] in
                self?.setEnabled(true, for: item)
                self?.tableView.reloadData()
            }
        } else {
            setEnabled(enabled, for: item)
        }
    }

    private func setEnabled(_ enabled: Bool, for item: Item) {
        switch item {
        case .debugPanel:
            store.setDebugPanelEnabled(enabled)
        case .packetDiagnostics:
            store.setPacketFlowDiagnosticsEnabled(enabled)
        case .packetConsoleLogging:
            store.setPacketFlowConsoleLoggingEnabled(enabled)
        case .packetTelemetry:
            store.setPacketFlowTelemetryEnabled(enabled)
        }
    }
}
