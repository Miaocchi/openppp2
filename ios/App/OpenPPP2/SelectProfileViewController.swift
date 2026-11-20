import UIKit

// MARK: - Profile Selector

final class SelectProfileViewController: ProfileBrowserViewController {
    override func viewDidLoad() {
        super.viewDidLoad()
        title = L10n.tr("profiles.selectTitle")
        navigationItem.leftBarButtonItem = UIBarButtonItem(barButtonSystemItem: .close, target: self, action: #selector(close))
        navigationItem.rightBarButtonItem = UIBarButtonItem(barButtonSystemItem: .add, target: self, action: #selector(addProfile))
    }

    @objc private func close() {
        dismiss(animated: true)
    }


    override func tableView(_ tableView: UITableView, didSelectRowAt indexPath: IndexPath) {
        tableView.deselectRow(at: indexPath, animated: true)
        guard let profile = profile(at: indexPath) else { recoverEmptyList(); return }
        store.setActive(profile.id)
        promptRestartForActiveTunnelIfNeeded(
            profile: profile,
            message: L10n.format("profiles.switched.message", profile.name),
            noRestartMessage: nil,
            completion: { [weak self] in
                self?.dismiss(animated: true)
            }
        )
    }

    override func tableView(_ tableView: UITableView, trailingSwipeActionsConfigurationForRowAt indexPath: IndexPath) -> UISwipeActionsConfiguration? {
        guard let profile = profile(at: indexPath) else { return nil }
        let favorite = UIContextualAction(style: .normal, title: profile.favorite ? L10n.tr("profiles.unfavorite") : L10n.tr("profiles.favorite")) { [weak self] _, _, done in
            self?.toggleFavorite(profile)
            done(true)
        }
        favorite.backgroundColor = .systemYellow

        let edit = UIContextualAction(style: .normal, title: L10n.tr("common.edit")) { [weak self] _, _, done in
            self?.navigationController?.pushViewController(ProfileEditViewController(profile: profile), animated: true)
            done(true)
        }
        edit.backgroundColor = AppTheme.primary
        return UISwipeActionsConfiguration(actions: [edit, favorite])
    }

    private func toggleFavorite(_ profile: ConfigProfile) {
        var updated = profile
        updated.favorite.toggle()
        store.update(updated, snapshot: false)
    }
}
