#include <ppp/app/multipath/Session.h>

#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <algorithm>
#include <limits>

namespace ppp::app::multipath {
namespace {
bool ValidBinding(const SessionPathBinding* binding) noexcept {
    return binding == nullptr || (binding->sink != nullptr && !binding->path_id.empty() &&
        binding->path_id.size() <= 128);
}

TimePoint AddBounded(TimePoint now, Duration duration) noexcept {
    const Duration remaining = TimePoint::max() - now;
    return duration > remaining ? TimePoint::max() : now + duration;
}
}

CleansedSecret::CleansedSecret(const Secret32& value) noexcept : bytes_(value), set_(true) {}
CleansedSecret::~CleansedSecret() noexcept { Clear(); }
CleansedSecret::CleansedSecret(CleansedSecret&& other) noexcept : bytes_(other.bytes_), set_(other.set_) {
    other.Clear();
}
CleansedSecret& CleansedSecret::operator=(CleansedSecret&& other) noexcept {
    if (this != &other) { Clear(); bytes_ = other.bytes_; set_ = other.set_; other.Clear(); }
    return *this;
}
void CleansedSecret::Clear() noexcept { OPENSSL_cleanse(bytes_.data(), bytes_.size()); set_ = false; }

bool SessionState::Create(const std::string& identity, const SessionId& client_session_id,
                          TimePoint now, Duration requested_grace,
                          SessionTicket& ticket) noexcept {
    if (status_ != SessionStatus::Closed || identity.empty() || identity.size() > 128 ||
        requested_grace <= Duration::zero()) return false;

    SessionId new_id{};
    Secret32 new_credential{};
    if (RAND_bytes(new_id.data(), static_cast<int>(new_id.size())) != 1 ||
        RAND_bytes(new_credential.data(), static_cast<int>(new_credential.size())) != 1) {
        OPENSSL_cleanse(new_id.data(), new_id.size());
        OPENSSL_cleanse(new_credential.data(), new_credential.size());
        return false;
    }
    std::string prepared_identity;
    try { prepared_identity = identity; }
    catch (...) {
        OPENSSL_cleanse(new_id.data(), new_id.size());
        OPENSSL_cleanse(new_credential.data(), new_credential.size());
        return false;
    }
    Duration grace = std::min(requested_grace,
        std::chrono::duration_cast<Duration>(MaxResumeGrace));
    if (grace <= Duration::zero()) grace = Duration{1};
    id_ = new_id;
    client_session_id_ = client_session_id;
    credential_ = CleansedSecret(new_credential);
    identity_.swap(prepared_identity);
    status_ = SessionStatus::Active;
    generation_ = 1;
    grace_ = grace;
    deadline_ = TimePoint{};
    ticket.id = id_;
    ticket.client_session_id = client_session_id_;
    ticket.generation = generation_;
    ticket.credential = CleansedSecret(new_credential);
    ticket.grace = grace_;
    OPENSSL_cleanse(new_id.data(), new_id.size());
    OPENSSL_cleanse(new_credential.data(), new_credential.size());
    return true;
}

SessionSnapshot SessionState::Snapshot() const noexcept {
    return SessionSnapshot{id_, client_session_id_, generation_, status_, deadline_, carriers_.size()};
}

void SessionState::ExpireIfDue(TimePoint now) noexcept {
    if (status_ == SessionStatus::Resuming && now >= resume_attempt_deadline_) {
        status_ = SessionStatus::Suspended;
        resume_request_id_.clear();
        resume_attempt_deadline_ = TimePoint{};
    }
    if ((status_ == SessionStatus::Suspended || status_ == SessionStatus::Resuming) &&
        now >= deadline_) {
        status_ = SessionStatus::Expired;
        resume_request_id_.clear();
        resume_attempt_deadline_ = TimePoint{};
        carriers_.clear();
        credential_.Clear();
    }
}

bool SessionState::OnCarrierJoined(const std::string& carrier_id, std::uint64_t session_gen,
                                   std::uint64_t conn_gen,
                                   TimePoint now,
                                   const SessionPathBinding* binding) noexcept {
    if (!ValidBinding(binding)) return false;
    ExpireIfDue(now);
    if (status_ != SessionStatus::Active || session_gen != generation_ ||
        carrier_id.empty() || carrier_id.size() > 128 ||
        carriers_.size() >= MaxCarriers || carriers_.find(carrier_id) != carriers_.end()) return false;
    try {
        if (!carriers_.emplace(carrier_id, Carrier{session_gen, conn_gen}).second) return false;
    } catch (...) { return false; }
    if (binding != nullptr && binding->sink != nullptr) {
        PathEventEnvelope event;
        event.path_id = binding->path_id;
        event.epoch = binding->epoch;
        event.generation = binding->generation;
        event.monotonic_ms = binding->monotonic_ms;
        event.kind = PathEvent::Joined;
        (void)binding->sink->Publish(event);
    }
    return true;
}

bool SessionState::OnCarrierLeft(const std::string& carrier_id, std::uint64_t session_gen,
                                 std::uint64_t conn_gen,
                                 TimePoint now,
                                 const SessionPathBinding* binding) noexcept {
    if (!ValidBinding(binding)) return false;
    ExpireIfDue(now);
    auto found = carriers_.find(carrier_id);
    if (found == carriers_.end() || found->second.session_generation != session_gen ||
        found->second.connection_generation != conn_gen ||
        (status_ != SessionStatus::Active && status_ != SessionStatus::Resuming)) return false;
    carriers_.erase(found);
    if (carriers_.empty()) {
        deadline_ = AddBounded(now, grace_);
        status_ = SessionStatus::Suspended;
        resume_request_id_.clear();
    }
    if (binding != nullptr && binding->sink != nullptr) {
        PathEventEnvelope event;
        event.path_id = binding->path_id;
        event.epoch = binding->epoch;
        event.generation = binding->generation;
        event.monotonic_ms = binding->monotonic_ms;
        event.kind = PathEvent::CarrierLost;
        (void)binding->sink->Publish(event);
    }
    return true;
}

bool SessionState::BeginResume(const SessionId& id, std::uint64_t expected_generation,
                               const std::string& identity, const std::string& request_id,
                               TimePoint now) noexcept {
    ExpireIfDue(now);
    if (status_ != SessionStatus::Suspended || now >= deadline_ || id != id_ ||
        expected_generation != generation_ || identity != identity_ || request_id.empty() ||
        request_id.size() > 64) return false;
    std::string prepared_request_id;
    try { prepared_request_id = request_id; }
    catch (...) { return false; }
    status_ = SessionStatus::Resuming;
    resume_request_id_.swap(prepared_request_id);
    resume_attempt_deadline_ = std::min(deadline_, AddBounded(now,
        std::chrono::duration_cast<Duration>(MaxResumeAttemptTtl)));
    return true;
}

bool SessionState::CommitResume(const std::string& request_id, const std::string& carrier_id,
                                std::uint64_t conn_gen, TimePoint now,
                                const SessionPathBinding* binding) noexcept {
    if (!ValidBinding(binding)) return false;
    ExpireIfDue(now);
    if (status_ != SessionStatus::Resuming || request_id != resume_request_id_ ||
        carrier_id.empty() || carrier_id.size() > 128 || carriers_.size() >= MaxCarriers ||
        carriers_.find(carrier_id) != carriers_.end() || generation_ == std::numeric_limits<std::uint64_t>::max() ||
        now >= deadline_) return false;
    try {
        if (!carriers_.emplace(carrier_id, Carrier{generation_ + 1, conn_gen}).second) return false;
    } catch (...) { return false; }
    ++generation_;
    deadline_ = TimePoint{};
    resume_attempt_deadline_ = TimePoint{};
    resume_request_id_.clear();
    status_ = SessionStatus::Active;
    if (binding != nullptr && binding->sink != nullptr) {
        PathEventEnvelope event;
        event.path_id = binding->path_id;
        event.epoch = binding->epoch;
        event.generation = binding->generation;
        event.monotonic_ms = binding->monotonic_ms;
        event.kind = PathEvent::Joined;
        (void)binding->sink->Publish(event);
    }
    return true;
}

bool SessionState::AbortResume(const std::string& request_id, TimePoint now) noexcept {
    ExpireIfDue(now);
    if (status_ != SessionStatus::Resuming || request_id != resume_request_id_) return false;
    resume_request_id_.clear();
    resume_attempt_deadline_ = TimePoint{};
    status_ = SessionStatus::Suspended;
    return true;
}

bool SessionState::Expire(TimePoint now) noexcept {
    const auto before = status_;
    ExpireIfDue(now);
    return before != status_ && status_ == SessionStatus::Expired;
}

void SessionState::ClearState() noexcept {
    credential_.Clear();
    OPENSSL_cleanse(id_.data(), id_.size());
    OPENSSL_cleanse(client_session_id_.data(), client_session_id_.size());
    identity_.clear();
    carriers_.clear();
    resume_request_id_.clear();
    generation_ = 0;
    grace_ = Duration::zero();
    deadline_ = TimePoint{};
    resume_attempt_deadline_ = TimePoint{};
}

bool SessionState::Close() noexcept {
    if (status_ == SessionStatus::Closed) return false;
    ClearState();
    status_ = SessionStatus::Closed;
    return true;
}

} // namespace ppp::app::multipath
