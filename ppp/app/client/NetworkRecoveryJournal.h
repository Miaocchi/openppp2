#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ppp::app::client {

// Durable write-ahead state for network resources modified by the VPN.
// Entries remain present until the caller has verified restoration.
class NetworkRecoveryJournal final {
public:
    enum class Stage {
        DirectoryCreate,
        Lock,
        Write,
        Flush,
        Rename,
        DirectoryFlush,
    };

    struct Options final {
        // Returning false injects an I/O failure before the named stage.
        std::function<bool(Stage)> allow_stage;
    };

    struct Entry final {
        std::string resource;
        std::string target;
        std::string before;
        std::string after;
        bool confirmed = false;
    };

    NetworkRecoveryJournal() = default;
    ~NetworkRecoveryJournal();
    NetworkRecoveryJournal(const NetworkRecoveryJournal&) = delete;
    NetworkRecoveryJournal& operator=(const NetworkRecoveryJournal&) = delete;

    // Opens (and locks) directory/network-recovery.journal. Existing records
    // are validated before the journal is made available to the caller.
    bool Open(const std::string& directory, const std::string& owner,
              const Options& options = {}, std::string* error = nullptr);
    void Close() noexcept;

    // Record the state needed to undo an operation before changing the OS.
    // Re-recording the same resource/target replaces its previous intent.
    bool RecordIntent(const std::string& resource, const std::string& target,
                      const std::string& before, const std::string& after,
                      std::string* error = nullptr);
    // Marks an apply operation complete. The entry is still pending restore.
    bool Confirm(const std::string& resource, const std::string& target,
                std::string* error = nullptr);
    // Removes an entry only after restoration has been verified.
    bool Remove(const std::string& resource, const std::string& target,
                std::string* error = nullptr);

    std::vector<Entry> Pending() const;
    bool HasPending() const;
    bool IsOpen() const noexcept { return open_; }

private:
    bool StageAllowed(Stage stage) const;
    bool Rewrite(std::string* error);
    void CloseWithLock() noexcept;

    std::string directory_;
    std::string owner_;
    Options options_;
    std::vector<Entry> entries_;
    intptr_t lock_handle_ = -1;
    bool open_ = false;
};

} // namespace ppp::app::client
