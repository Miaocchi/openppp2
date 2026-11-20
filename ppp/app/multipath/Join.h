#pragma once

#include <ppp/app/multipath/Session.h>

#include <array>
#include <map>
#include <string>

namespace ppp::app::multipath {

inline constexpr std::size_t MaxIdentityBytes = 128;
inline constexpr std::size_t MaxRequestIdBytes = 64;
inline constexpr std::size_t MaxCarrierIdBytes = 128;
inline constexpr std::size_t MaxJoinTransactions = 64;
inline constexpr std::chrono::seconds JoinTransactionTtl{10};

enum class JoinOperation : std::uint8_t { Join = 1, Resume = 2 };
enum class JoinPhase : std::uint8_t { Reserve = 1, Confirm = 2, Commit = 3 };
enum class JoinResult : std::uint8_t {
    Accepted, Duplicate, Committed, NotFound, Rejected, Expired, Capacity
};

/** Fixed-size cryptographic context plus bounded identifiers; suitable for a wire codec. */
struct JoinRequest final {
    JoinOperation operation = JoinOperation::Join;
    JoinPhase phase = JoinPhase::Reserve;
    std::string authenticated_identity;
    std::string request_id;
    std::string carrier_id;
    SessionId session_id{};
    SessionId client_session_id{};
    std::uint64_t generation = 0;
    std::uint64_t peer_committed_operation_sequence = 0;
    Secret32 client_nonce{};
    Secret32 server_nonce{};
    Secret32 candidate_binding{};
    Secret32 proof{};
};

struct JoinResponse final {
    JoinResult result = JoinResult::Rejected;
    Secret32 server_nonce{};
    Secret32 proof{};
    std::uint64_t generation = 0;
    std::uint64_t server_committed_operation_sequence = 0;
};

bool ComputeJoinProof(const Secret32& credential, const JoinRequest& request,
                      Secret32& proof) noexcept;
bool ComputeJoinResponseProof(const Secret32& credential, const JoinRequest& request,
    std::uint64_t server_committed_operation_sequence, Secret32& proof) noexcept;
bool VerifyJoinProof(const Secret32& credential, const JoinRequest& request) noexcept;
bool VerifyJoinResponseProof(const Secret32& credential, const JoinRequest& request,
    std::uint64_t server_committed_operation_sequence, const Secret32& proof) noexcept;

/** Owner-serialized, bounded reserve/confirm/commit/query coordinator. */
class JoinCoordinator final {
public:
    JoinResult Reserve(SessionState& session, const JoinRequest& request,
                       TimePoint now, JoinResponse& response,
                       std::uint64_t server_committed_operation_sequence = 0,
                       std::uint64_t maximum_peer_committed_operation_sequence = 0) noexcept;
    JoinResult Confirm(SessionState& session, const JoinRequest& request, TimePoint now,
                       JoinResponse& response) noexcept;
    JoinResult Commit(SessionState& session, const JoinRequest& request,
                      std::uint64_t connection_generation, TimePoint now,
                      JoinResponse& response,
                      const SessionPathBinding* binding = nullptr) noexcept;
    JoinResult Query(const std::string& authenticated_identity,
                     const std::string& request_id, TimePoint now,
                     JoinResponse& response) noexcept;
    void Cancel(const std::string& request_id) noexcept;
    std::size_t PendingCount() const noexcept { return transactions_.size(); }

private:
    enum class Phase : std::uint8_t { Reserved, Confirmed, Committed };
    struct Transaction {
        Transaction() = default;
        ~Transaction() noexcept;
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;
        Transaction(Transaction&&) noexcept;
        Transaction& operator=(Transaction&&) = delete;
        JoinRequest request;
        Secret32 server_nonce{};
        Phase phase = Phase::Reserved;
        TimePoint expires{};
        std::uint64_t server_committed_operation_sequence = 0;
        JoinResponse last_response{};
        JoinResponse final_response{};
    };
    void Prune(TimePoint now) noexcept;
    std::map<std::string, Transaction> transactions_;
};

} // namespace ppp::app::multipath
