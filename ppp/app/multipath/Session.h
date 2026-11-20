#pragma once

#include <ppp/app/multipath/PathControlTypes.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace ppp::app::multipath {

using SessionId = std::array<std::uint8_t, 16>;
using Secret32 = std::array<std::uint8_t, 32>;
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration = Clock::duration;
inline constexpr std::size_t MaxCarriers = 32;
inline constexpr std::chrono::seconds MaxResumeGrace{30};
inline constexpr std::chrono::seconds MaxResumeAttemptTtl{10};

/** Move-only secret bytes. Copies are explicitly cleansed on destruction. */
class CleansedSecret final {
public:
    CleansedSecret() noexcept = default;
    explicit CleansedSecret(const Secret32& value) noexcept;
    ~CleansedSecret() noexcept;
    CleansedSecret(const CleansedSecret&) = delete;
    CleansedSecret& operator=(const CleansedSecret&) = delete;
    CleansedSecret(CleansedSecret&& other) noexcept;
    CleansedSecret& operator=(CleansedSecret&& other) noexcept;
    const Secret32& Value() const noexcept { return bytes_; }
    bool IsSet() const noexcept { return set_; }
    void Clear() noexcept;
private:
    Secret32 bytes_{};
    bool set_ = false;
};

struct SessionTicket final {
    SessionId id{};
    SessionId client_session_id{};
    std::uint64_t generation = 0;
    CleansedSecret credential;
    Duration grace{};
    SessionTicket() noexcept = default;
    SessionTicket(const SessionTicket&) = delete;
    SessionTicket& operator=(const SessionTicket&) = delete;
    SessionTicket(SessionTicket&&) noexcept = default;
    SessionTicket& operator=(SessionTicket&&) noexcept = default;
};

enum class SessionStatus : std::uint8_t { Active, Suspended, Resuming, Expired, Closed };

inline PathState PathStateForSessionStatus(SessionStatus status) noexcept {
    switch (status) {
    case SessionStatus::Active: return PathState::Active;
    case SessionStatus::Suspended: return PathState::Degraded;
    case SessionStatus::Resuming: return PathState::Warming;
    case SessionStatus::Expired:
    case SessionStatus::Closed: return PathState::Stopped;
    }
    return PathState::Stopped;
}

struct SessionSnapshot final {
    SessionId id{};
    SessionId client_session_id{};
    std::uint64_t generation = 0;
    SessionStatus status = SessionStatus::Closed;
    TimePoint deadline{};
    std::size_t carrier_count = 0;
};

/**
 * Single-owner state. All methods, including callbacks, must be called on the
 * owning serialized executor. Candidate handshakes must marshal results there.
 */
class SessionState final {
public:
    bool Create(const std::string& authenticated_identity, const SessionId& client_session_id,
                TimePoint now,
                Duration requested_grace, SessionTicket& ticket) noexcept;
    SessionSnapshot Snapshot() const noexcept;
    PathState ControlState() const noexcept { return PathStateForSessionStatus(status_); }
    const std::string& AuthenticatedIdentity() const noexcept { return identity_; }
    const CleansedSecret& Credential() const noexcept { return credential_; }

    bool OnCarrierJoined(const std::string& carrier_id, std::uint64_t session_generation,
                         std::uint64_t connection_generation,
                         TimePoint now,
                         const SessionPathBinding* binding = nullptr) noexcept;
    bool OnCarrierLeft(const std::string& carrier_id, std::uint64_t session_generation,
                       std::uint64_t connection_generation,
                       TimePoint now,
                       const SessionPathBinding* binding = nullptr) noexcept;
    bool BeginResume(const SessionId& id, std::uint64_t expected_generation,
                     const std::string& authenticated_identity,
                     const std::string& request_id, TimePoint now) noexcept;
    bool CommitResume(const std::string& request_id, const std::string& carrier_id,
                      std::uint64_t connection_generation, TimePoint now,
                      const SessionPathBinding* binding = nullptr) noexcept;
    bool AbortResume(const std::string& request_id, TimePoint now) noexcept;
    bool Expire(TimePoint now) noexcept;
    bool Close() noexcept;

private:
    struct Carrier { std::uint64_t session_generation; std::uint64_t connection_generation; };
    void ExpireIfDue(TimePoint now) noexcept;
    void ClearState() noexcept;

    std::string identity_;
    SessionId id_{};
    SessionId client_session_id_{};
    CleansedSecret credential_;
    SessionStatus status_ = SessionStatus::Closed;
    std::uint64_t generation_ = 0;
    Duration grace_{};
    TimePoint deadline_{};
    TimePoint resume_attempt_deadline_{};
    std::map<std::string, Carrier> carriers_;
    std::string resume_request_id_;
};

} // namespace ppp::app::multipath
