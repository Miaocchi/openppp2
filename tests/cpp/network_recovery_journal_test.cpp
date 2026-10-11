#include <ppp/app/client/NetworkRecoveryJournal.h>

#include <cassert>
#include <filesystem>
#include <iostream>

using ppp::app::client::NetworkRecoveryJournal;

int main() {
    const auto root = std::filesystem::temp_directory_path() / "openppp2-network-recovery-journal-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    NetworkRecoveryJournal journal;
    assert(journal.Open(root.string(), "test-owner"));
    assert(journal.RecordIntent("dns", "if:7", "dhcp", "1.1.1.1"));
    assert(journal.HasPending());
    assert(journal.Pending().front().before == "dhcp");
    assert(journal.Confirm("dns", "if:7"));
    journal.Close();

    NetworkRecoveryJournal reopened;
    assert(reopened.Open(root.string(), "test-owner"));
    auto pending = reopened.Pending();
    assert(pending.size() == 1 && pending.front().confirmed);
    assert(reopened.Remove("dns", "if:7"));
    reopened.Close();

    NetworkRecoveryJournal empty;
    assert(empty.Open(root.string(), "test-owner"));
    assert(!empty.HasPending());
    empty.Close();

    NetworkRecoveryJournal faulted;
    NetworkRecoveryJournal::Options options;
    options.allow_stage = [](NetworkRecoveryJournal::Stage stage) {
        return stage != NetworkRecoveryJournal::Stage::Rename;
    };
    assert(faulted.Open(root.string(), "test-owner", options));
    assert(!faulted.RecordIntent("route", "if:7", "old", "new"));
    assert(!faulted.HasPending());
    faulted.Close();

    NetworkRecoveryJournal rewrite_fault;
    assert(rewrite_fault.Open(root.string(), "test-owner"));
    assert(rewrite_fault.RecordIntent("route", "if:7", "old", "new"));
    rewrite_fault.Close();
    NetworkRecoveryJournal rewrite_retry;
    assert(rewrite_retry.Open(root.string(), "test-owner", options));
    assert(!rewrite_retry.RecordIntent("route", "if:7", "mutated", "value"));
    assert(rewrite_retry.Pending().front().before == "old");
    rewrite_retry.Close();

    NetworkRecoveryJournal wrong_owner;
    assert(!wrong_owner.Open(root.string(), "other-owner"));
    std::filesystem::remove_all(root, ec);
    std::cout << "network recovery journal tests passed\n";
}
