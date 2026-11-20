#include <ppp/app/multipath/BypassLease.h>

#include <algorithm>

namespace ppp::app::multipath {
    struct BypassLeaseManager::State final {
        struct Entry final {
            enum class Status { Acquiring, Active, Releasing };
            Address address;
            std::size_t references = 0;
            Status status = Status::Acquiring;
        };

        RouteLeaseCallbacks callbacks;
        Limits limits;
        mutable std::mutex mutex;
        mutable std::condition_variable changed;
        std::unordered_map<std::string, Entry> entries;
        bool callbacks_valid = true;

        State(const RouteLeaseCallbacks& requested_callbacks, const Limits& requested_limits)
            : callbacks(requested_callbacks), limits(requested_limits),
              callbacks_valid(static_cast<bool>(callbacks.acquire) == static_cast<bool>(callbacks.release)) {}
    };

    BypassLeaseManager::Lease::Lease(
        const std::shared_ptr<State>& state,
        const Address& address,
        std::string key) noexcept
        : state_(state), address_(address), key_(std::move(key)) {}

    BypassLeaseManager::Lease::Lease(Lease&& other) noexcept
        : state_(std::move(other.state_)), address_(std::move(other.address_)), key_(std::move(other.key_)) {}

    BypassLeaseManager::Lease& BypassLeaseManager::Lease::operator=(Lease&& other) noexcept {
        if (this != &other) {
            Reset();
            state_ = std::move(other.state_);
            address_ = std::move(other.address_);
            key_ = std::move(other.key_);
        }
        return *this;
    }

    BypassLeaseManager::Lease::~Lease() noexcept {
        Reset();
    }

    void BypassLeaseManager::Lease::Reset() noexcept {
        std::shared_ptr<State> state = std::move(state_);
        std::string key = std::move(key_);
        address_ = Address();
        if (state) {
            BypassLeaseManager::Release(state, key);
        }
    }

    BypassLeaseManager::BypassLeaseManager() noexcept
        : BypassLeaseManager(RouteLeaseCallbacks(), Limits()) {}

    BypassLeaseManager::BypassLeaseManager(const RouteLeaseCallbacks& callbacks) noexcept
        : BypassLeaseManager(callbacks, Limits()) {}

    BypassLeaseManager::BypassLeaseManager(
        const RouteLeaseCallbacks& callbacks,
        const Limits& limits) noexcept {
        try {
            state_ = std::make_shared<State>(callbacks, limits);
        }
        catch (...) {
            state_.reset();
        }
    }

    bool BypassLeaseManager::MakeKey(const Address& address, std::string& key) noexcept {
        if ((!address.is_v4() && !address.is_v6()) || address.is_unspecified() || address.is_multicast()) {
            return false;
        }
        try {
            std::string value = address.to_string();
            if (value.empty()) {
                return false;
            }
            key = std::move(value);
            return true;
        }
        catch (...) {
            return false;
        }
    }

    bool BypassLeaseManager::AcquireAddress(const Address& address, Lease& output) noexcept {
        std::shared_ptr<State> state = state_;
        if (!state) {
            return false;
        }

        std::string key;
        if (!MakeKey(address, key)) {
            return false;
        }

        bool needs_callback = false;
        try {
            {
                std::unique_lock<std::mutex> lock(state->mutex);
                for (;;) {
                    if (!state->callbacks_valid) {
                        return false;
                    }
                    auto found = state->entries.find(key);
                    if (found != state->entries.end()) {
                        if (found->second.status != State::Entry::Status::Active) {
                            state->changed.wait(lock);
                            continue;
                        }
                        if (found->second.references >= state->limits.max_references_per_address) {
                            return false;
                        }
                        ++found->second.references;
                        break;
                    }
                    if (state->entries.size() >= state->limits.max_tracked_addresses) {
                        return false;
                    }
                    state->entries.emplace(key, State::Entry{address, 1, State::Entry::Status::Acquiring});
                    needs_callback = true;
                    break;
                }
            }

            if (needs_callback && state->callbacks.acquire) {
                bool succeeded = false;
                try { succeeded = state->callbacks.acquire(address); } catch (...) {}
                std::lock_guard<std::mutex> lock(state->mutex);
                auto found = state->entries.find(key);
                if (found == state->entries.end() || found->second.status != State::Entry::Status::Acquiring) {
                    state->changed.notify_all();
                    return false;
                }
                if (!succeeded) {
                    state->entries.erase(found);
                    state->changed.notify_all();
                    return false;
                }
                found->second.status = State::Entry::Status::Active;
                state->changed.notify_all();
            }
            else if (needs_callback) {
                std::lock_guard<std::mutex> lock(state->mutex);
                auto found = state->entries.find(key);
                if (found == state->entries.end()) {
                    state->changed.notify_all();
                    return false;
                }
                found->second.status = State::Entry::Status::Active;
                state->changed.notify_all();
            }

            Lease acquired(state, address, std::move(key));
            output = std::move(acquired);
            return true;
        }
        catch (...) {
            if (needs_callback) {
                std::lock_guard<std::mutex> lock(state->mutex);
                auto found = state->entries.find(key);
                if (found != state->entries.end() && found->second.status == State::Entry::Status::Acquiring) {
                    state->entries.erase(found);
                    state->changed.notify_all();
                }
            }
            return false;
        }
    }

    bool BypassLeaseManager::AcquireCandidates(
        const std::vector<Address>& addresses,
        LeaseSet& output) noexcept {
        if (addresses.empty() || addresses.size() > MaxDnsAddressesPerPath) {
            return false;
        }

        try {
            LeaseSet replacement;
            replacement.leases_.reserve(std::min(addresses.size(), MaxDnsAddressesPerPath));
            std::unordered_map<std::string, bool> seen;
            seen.reserve(std::min(addresses.size(), MaxDnsAddressesPerPath));
            for (const Address& address : addresses) {
                std::string key;
                if (!MakeKey(address, key)) {
                    return false;
                }
                if (!seen.emplace(key, true).second) {
                    continue;
                }
                Lease lease;
                if (!AcquireAddress(address, lease)) {
                    return false;
                }
                replacement.leases_.emplace_back(std::move(lease));
            }
            if (replacement.Empty()) {
                return false;
            }
            output = std::move(replacement);
            return true;
        }
        catch (...) {
            return false;
        }
    }

    std::size_t BypassLeaseManager::TrackedAddressCount() const noexcept {
        std::shared_ptr<State> state = state_;
        if (!state) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        return state->entries.size();
    }

    std::size_t BypassLeaseManager::ReferenceCount(const Address& address) const noexcept {
        std::shared_ptr<State> state = state_;
        std::string key;
        if (!state || !MakeKey(address, key)) {
            return 0;
        }
        std::lock_guard<std::mutex> lock(state->mutex);
        auto found = state->entries.find(key);
        return found == state->entries.end() ? 0 : found->second.references;
    }

    void BypassLeaseManager::Release(
        const std::shared_ptr<State>& state,
        const std::string& key) noexcept {
        if (!state || key.empty()) {
            return;
        }
        Address address;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            auto found = state->entries.find(key);
            if (found == state->entries.end() || found->second.status != State::Entry::Status::Active ||
                found->second.references == 0) {
                return;
            }
            if (--found->second.references != 0) {
                return;
            }
            address = found->second.address;
            found->second.status = State::Entry::Status::Releasing;
        }
        if (state->callbacks.release) {
            try { state->callbacks.release(address); } catch (...) {}
        }
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            auto found = state->entries.find(key);
            if (found != state->entries.end() && found->second.status == State::Entry::Status::Releasing) {
                state->entries.erase(found);
            }
            state->changed.notify_all();
        }
    }
}
