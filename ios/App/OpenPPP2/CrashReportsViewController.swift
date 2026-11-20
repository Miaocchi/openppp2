import UIKit

final class CrashReportsViewController: UITableViewController {
    private let vpn = VPNController.shared
    private var snapshots: [CrashReporter.StoreSnapshot] = []
    private var loadingExtension = false
    private var actionSection: Int { 1 + CrashReporter.ProcessKind.allCases.count }
    private var hasReports: Bool { snapshots.contains { !$0.reportIDs.isEmpty } }
    private var uploading = false

    override func viewDidLoad() {
        super.viewDidLoad()
        title = L10n.tr("settings.crashReports")
        tableView.backgroundColor = .systemGroupedBackground
        tableView.register(UITableViewCell.self, forCellReuseIdentifier: "Cell")
        navigationItem.rightBarButtonItem = UIBarButtonItem(barButtonSystemItem: .refresh, target: self, action: #selector(reloadReports))
    }

    override func viewWillAppear(_ animated: Bool) {
        super.viewWillAppear(animated)
        reloadReports()
    }

    @objc private func reloadReports() {
        guard !loadingExtension else { return }
        snapshots = CrashReporter.storeSnapshots
        loadingExtension = !CrashReporter.isSharedContainerAvailable
        tableView.reloadData()
        guard !CrashReporter.isSharedContainerAvailable else { return }

        vpn.fetchPacketTunnelCrashReports { [weak self] snapshot in
            guard let self else { return }
            self.loadingExtension = false
            if let snapshot {
                self.upsert(snapshot)
                self.tableView.reloadData()
            } else {
                self.removeSnapshot(for: .packetTunnel)
                self.tableView.reloadData()
            }
        }
    }

    override func numberOfSections(in tableView: UITableView) -> Int {
        actionSection + 1
    }

    override func tableView(_ tableView: UITableView, numberOfRowsInSection section: Int) -> Int {
        if section == 0 || section == actionSection { return 2 }
        guard let snapshot = snapshot(forSection: section) else {
            return 1
        }
        return max(snapshot.reportCount, 1)
    }

    override func tableView(_ tableView: UITableView, titleForHeaderInSection section: Int) -> String? {
        if section == 0 { return L10n.tr("settings.section.storage") }
        if section == actionSection { return L10n.tr("crash.manage") }
        guard let snapshot = snapshot(forSection: section) else {
            return L10n.format("crash.pendingHeader", CrashReporter.ProcessKind.packetTunnel.localizedDisplayName)
        }
        return L10n.format("crash.pendingHeader", snapshot.process.localizedDisplayName)
    }

    override func tableView(_ tableView: UITableView, cellForRowAt indexPath: IndexPath) -> UITableViewCell {
        let cell = tableView.dequeueReusableCell(withIdentifier: "Cell", for: indexPath)
        var config = UIListContentConfiguration.subtitleCell()
        config.secondaryTextProperties.numberOfLines = 0
        cell.accessoryType = .none
        let actionable = indexPath.section == actionSection && hasReports && !uploading
        cell.selectionStyle = actionable ? .default : .none
        cell.accessibilityTraits = .staticText

        if indexPath.section == 0 {
            config.text = L10n.tr(indexPath.row == 0 ? "crash.status" : "crash.storage")
            config.secondaryText = indexPath.row == 0 ? localizedCrashReportSummary(for: snapshots) : localizedCrashStorageDescription()
        } else if indexPath.section == actionSection {
            let send = indexPath.row == 0
            config.text = L10n.tr(send ? "crash.uploadToOtel" : "crash.clearLocal")
            config.textProperties.color = actionable ? (send ? AppTheme.primary : .systemRed) : .secondaryLabel
            config.secondaryText = L10n.tr(uploading ? "crash.uploading" : (send ? "crash.uploadDeletesLocal" : "crash.clearLocal.detail"))
            cell.accessibilityTraits = actionable ? [.button] : [.button, .notEnabled]
        } else {
            guard let snapshot = snapshot(forSection: indexPath.section) else {
                config.text = L10n.tr(loadingExtension ? "crash.checking" : "crash.extensionUnavailable")
                config.secondaryText = L10n.tr("crash.extensionRecovery")
                cell.accessoryType = .none
                cell.contentConfiguration = config
                return cell
            }

            if snapshot.reportIDs.isEmpty {
                config.text = L10n.tr("crash.none")
                config.secondaryText = L10n.tr("crash.none.detail")
                cell.accessoryType = .none
            } else {
                let reportID = snapshot.reportIDs[indexPath.row]
                config.text = L10n.format("crash.reportID", String(reportID))
                config.secondaryText = L10n.tr("crash.waitingUpload")
                cell.accessoryType = .none
            }
        }

        cell.contentConfiguration = config
        return cell
    }

    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        guard indexPath.section == actionSection, hasReports, !uploading else { return }

        if indexPath.row == 0 {
            uploadCrashReports()
            return
        }

        let alert = UIAlertController(title: L10n.tr("crash.clear.title"), message: L10n.tr("crash.clear.message"), preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: L10n.tr("common.cancel"), style: .cancel))
        alert.addAction(UIAlertAction(title: L10n.tr("common.clear"), style: .destructive) { [weak self] _ in
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

    private func uploadCrashReports() {
        guard !uploading else { return }
        let settings = TelemetrySettingsStore.shared.settings()
        guard settings.canUpload, settings.includeCrashReports else {
            presentMessage(title: L10n.tr("crash.uploadDisabled.title"), message: L10n.tr("crash.uploadDisabled.message"))
            return
        }

        confirmReportUpload(to: settings.effectiveEndpoint, savesSettings: false) { [weak self] in
            self?.sendReports(settings)
        }
    }

    private func sendReports(_ settings: TelemetrySettings) {
        uploading = true
        tableView.reloadData()
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
                    summary.lastError = L10n.tr("crash.vpnReportUnavailable")
                }
                group.leave()
            }
        }

        group.notify(queue: .main) { [weak self] in
            guard let self else { return }
            self.uploading = false
            self.reloadReports()
            let detail = summary.lastError.map { "\n\($0)" } ?? ""
            self.presentMessage(title: L10n.tr("crash.uploadFinished"), message: summary.localizedDisplayText + detail)
        }
    }

    private func snapshot(forSection section: Int) -> CrashReporter.StoreSnapshot? {
        let processes = CrashReporter.ProcessKind.allCases
        guard processes.indices.contains(section - 1) else { return nil }
        return snapshots.first { $0.process == processes[section - 1] }
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
