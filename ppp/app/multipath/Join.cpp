#include <ppp/app/multipath/Join.h>

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstring>
#include <vector>
#include <utility>

namespace ppp::app::multipath {
namespace {
constexpr std::uint8_t Domain[] = {'o','p','e','n','p','p','p','2','-','m','p','-','j','o','i','n','-','v','1',0};
constexpr std::uint8_t ResponseDomain[] = {'s','e','r','v','e','r','-','r','e','s','p','o','n','s','e','-','v','1',0};

void Put16(std::vector<std::uint8_t>& out, std::size_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}
void Put64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 7; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}
bool Valid(const JoinRequest& r) noexcept {
    return (r.operation == JoinOperation::Join || r.operation == JoinOperation::Resume) &&
        (r.phase == JoinPhase::Reserve || r.phase == JoinPhase::Confirm || r.phase == JoinPhase::Commit) &&
        !r.authenticated_identity.empty() && r.authenticated_identity.size() <= MaxIdentityBytes &&
        !r.request_id.empty() && r.request_id.size() <= MaxRequestIdBytes &&
        !r.carrier_id.empty() && r.carrier_id.size() <= MaxCarrierIdBytes;
}
bool BuildTranscript(const JoinRequest& r, std::vector<std::uint8_t>& out) noexcept {
    if (!Valid(r)) return false;
    try {
        out.clear();
        out.insert(out.end(), std::begin(Domain), std::end(Domain));
        out.push_back(static_cast<std::uint8_t>(r.operation));
        out.push_back(static_cast<std::uint8_t>(r.phase));
        out.insert(out.end(), r.session_id.begin(), r.session_id.end());
        out.insert(out.end(), r.client_session_id.begin(), r.client_session_id.end());
        Put64(out, r.generation);
        Put64(out, r.peer_committed_operation_sequence);
        Put16(out, r.authenticated_identity.size());
        out.insert(out.end(), r.authenticated_identity.begin(), r.authenticated_identity.end());
        Put16(out, r.request_id.size());
        out.insert(out.end(), r.request_id.begin(), r.request_id.end());
        Put16(out, r.carrier_id.size());
        out.insert(out.end(), r.carrier_id.begin(), r.carrier_id.end());
        out.insert(out.end(), r.client_nonce.begin(), r.client_nonce.end());
        out.insert(out.end(), r.server_nonce.begin(), r.server_nonce.end());
        out.insert(out.end(), r.candidate_binding.begin(), r.candidate_binding.end());
        return out.size() <= 512;
    } catch (...) { out.clear(); return false; }
}
bool IsZero(const Secret32& value) noexcept {
    std::uint8_t v = 0;
    for (auto b : value) v |= b;
    return v == 0;
}
bool SameRequest(const JoinRequest& a, const JoinRequest& b) noexcept {
    return a.operation == b.operation && a.authenticated_identity == b.authenticated_identity &&
        a.request_id == b.request_id && a.carrier_id == b.carrier_id && a.session_id == b.session_id &&
        a.client_session_id == b.client_session_id && a.generation == b.generation && a.client_nonce == b.client_nonce &&
        a.peer_committed_operation_sequence == b.peer_committed_operation_sequence &&
        a.candidate_binding == b.candidate_binding;
}
bool SetProof(JoinResponse& response, const Secret32& key, JoinRequest request) noexcept {
    request.phase = JoinPhase::Confirm;
    request.server_nonce = response.server_nonce;
    return ComputeJoinResponseProof(key, request,
        response.server_committed_operation_sequence, response.proof);
}
} // namespace

JoinCoordinator::Transaction::~Transaction() noexcept {
    OPENSSL_cleanse(request.client_nonce.data(), request.client_nonce.size());
    OPENSSL_cleanse(request.server_nonce.data(), request.server_nonce.size());
    OPENSSL_cleanse(request.candidate_binding.data(), request.candidate_binding.size());
    OPENSSL_cleanse(request.proof.data(), request.proof.size());
    OPENSSL_cleanse(server_nonce.data(), server_nonce.size());
    OPENSSL_cleanse(final_response.server_nonce.data(), final_response.server_nonce.size());
    OPENSSL_cleanse(final_response.proof.data(), final_response.proof.size());
    OPENSSL_cleanse(last_response.server_nonce.data(), last_response.server_nonce.size());
    OPENSSL_cleanse(last_response.proof.data(), last_response.proof.size());
}

JoinCoordinator::Transaction::Transaction(Transaction&& other) noexcept
    : request(std::move(other.request)), server_nonce(other.server_nonce), phase(other.phase),
      expires(other.expires), server_committed_operation_sequence(other.server_committed_operation_sequence),
      last_response(other.last_response), final_response(other.final_response) {
    OPENSSL_cleanse(other.request.client_nonce.data(), other.request.client_nonce.size());
    OPENSSL_cleanse(other.request.server_nonce.data(), other.request.server_nonce.size());
    OPENSSL_cleanse(other.request.candidate_binding.data(), other.request.candidate_binding.size());
    OPENSSL_cleanse(other.request.proof.data(), other.request.proof.size());
    OPENSSL_cleanse(other.server_nonce.data(), other.server_nonce.size());
    OPENSSL_cleanse(other.final_response.server_nonce.data(), other.final_response.server_nonce.size());
    OPENSSL_cleanse(other.final_response.proof.data(), other.final_response.proof.size());
    OPENSSL_cleanse(other.last_response.server_nonce.data(), other.last_response.server_nonce.size());
    OPENSSL_cleanse(other.last_response.proof.data(), other.last_response.proof.size());
}

bool ComputeJoinProof(const Secret32& credential, const JoinRequest& request,
                      Secret32& proof) noexcept {
    std::vector<std::uint8_t> transcript;
    Secret32 temporary{};
    if (!BuildTranscript(request, transcript)) { OPENSSL_cleanse(proof.data(), proof.size()); return false; }
    unsigned int len = 0;
    const auto* result = HMAC(EVP_sha256(), credential.data(), static_cast<int>(credential.size()),
        transcript.data(), transcript.size(), temporary.data(), &len);
    const bool ok = result == temporary.data() && len == temporary.size();
    if (ok) proof = temporary;
    else OPENSSL_cleanse(proof.data(), proof.size());
    OPENSSL_cleanse(temporary.data(), temporary.size());
    OPENSSL_cleanse(transcript.data(), transcript.size());
    return ok;
}
bool VerifyJoinProof(const Secret32& credential, const JoinRequest& request) noexcept {
    Secret32 expected{};
    const bool computed = ComputeJoinProof(credential, request, expected);
    const bool matches = computed && CRYPTO_memcmp(expected.data(), request.proof.data(), expected.size()) == 0;
    OPENSSL_cleanse(expected.data(), expected.size());
    return matches;
}

bool ComputeJoinResponseProof(const Secret32& credential, const JoinRequest& request,
    std::uint64_t server_committed_operation_sequence, Secret32& proof) noexcept {
    std::vector<std::uint8_t> transcript;
    Secret32 temporary{};
    if (!BuildTranscript(request, transcript)) {
        OPENSSL_cleanse(proof.data(), proof.size());
        return false;
    }
    try {
        transcript.insert(transcript.end(), std::begin(ResponseDomain), std::end(ResponseDomain));
        Put64(transcript, server_committed_operation_sequence);
    } catch (...) {
        OPENSSL_cleanse(proof.data(), proof.size());
        OPENSSL_cleanse(transcript.data(), transcript.size());
        return false;
    }
    unsigned int len = 0;
    const auto* result = HMAC(EVP_sha256(), credential.data(), static_cast<int>(credential.size()),
        transcript.data(), transcript.size(), temporary.data(), &len);
    const bool ok = result == temporary.data() && len == temporary.size();
    if (ok) proof = temporary;
    else OPENSSL_cleanse(proof.data(), proof.size());
    OPENSSL_cleanse(temporary.data(), temporary.size());
    OPENSSL_cleanse(transcript.data(), transcript.size());
    return ok;
}

bool VerifyJoinResponseProof(const Secret32& credential, const JoinRequest& request,
    std::uint64_t server_committed_operation_sequence, const Secret32& proof) noexcept {
    Secret32 expected{};
    const bool computed = ComputeJoinResponseProof(credential, request,
        server_committed_operation_sequence, expected);
    const bool matches = computed && CRYPTO_memcmp(expected.data(), proof.data(), expected.size()) == 0;
    OPENSSL_cleanse(expected.data(), expected.size());
    return matches;
}

void JoinCoordinator::Prune(TimePoint now) noexcept {
    for (auto it = transactions_.begin(); it != transactions_.end();) {
        if (now >= it->second.expires) {
            OPENSSL_cleanse(it->second.server_nonce.data(), it->second.server_nonce.size());
            OPENSSL_cleanse(it->second.request.proof.data(), it->second.request.proof.size());
            it = transactions_.erase(it);
        } else ++it;
    }
}

JoinResult JoinCoordinator::Reserve(SessionState& session, const JoinRequest& r,
                                    TimePoint now, JoinResponse& response,
                                    std::uint64_t server_committed_operation_sequence,
                                    std::uint64_t maximum_peer_committed_operation_sequence) noexcept {
    response = JoinResponse{};
    Prune(now);
    if (!Valid(r) || r.phase != JoinPhase::Reserve || !IsZero(r.server_nonce) ||
        IsZero(r.client_nonce) || IsZero(r.candidate_binding) ||
        r.session_id != session.Snapshot().id || r.generation != session.Snapshot().generation ||
        r.client_session_id != session.Snapshot().client_session_id ||
        r.authenticated_identity != session.AuthenticatedIdentity() || !session.Credential().IsSet() ||
        r.peer_committed_operation_sequence > maximum_peer_committed_operation_sequence ||
        !VerifyJoinProof(session.Credential().Value(), r)) return response.result = JoinResult::Rejected;
    auto found = transactions_.find(r.request_id);
    if (found != transactions_.end()) {
        if (!SameRequest(found->second.request, r)) return response.result = JoinResult::Rejected;
        if (found->second.phase == Phase::Committed) {
            response = found->second.final_response;
            return response.result;
        }
        response.result = JoinResult::Duplicate;
        response.server_nonce = found->second.server_nonce;
        response.generation = r.generation;
        response.server_committed_operation_sequence = found->second.server_committed_operation_sequence;
        if (!SetProof(response, session.Credential().Value(), r))
            return response.result = JoinResult::Rejected;
        return response.result;
    }
    if (transactions_.size() >= MaxJoinTransactions) return response.result = JoinResult::Capacity;
    std::map<std::string, Transaction>::iterator inserted;
    try {
        Transaction tx;
        tx.request = r;
        tx.request.proof.fill(0);
        if (RAND_bytes(tx.server_nonce.data(), static_cast<int>(tx.server_nonce.size())) != 1)
            return response.result = JoinResult::Rejected;
        tx.expires = now + JoinTransactionTtl;
        tx.server_committed_operation_sequence = server_committed_operation_sequence;
        auto result = transactions_.emplace(r.request_id, std::move(tx));
        if (!result.second) return response.result = JoinResult::Rejected;
        inserted = result.first;
    } catch (...) { return response.result = JoinResult::Capacity; }
    if (r.operation == JoinOperation::Resume && !session.BeginResume(r.session_id, r.generation,
        r.authenticated_identity, r.request_id, now)) {
        OPENSSL_cleanse(inserted->second.server_nonce.data(), inserted->second.server_nonce.size());
        transactions_.erase(inserted);
        return response.result = JoinResult::Rejected;
    }
    response.result = JoinResult::Accepted;
    response.server_nonce = inserted->second.server_nonce;
    response.generation = r.generation;
    response.server_committed_operation_sequence = inserted->second.server_committed_operation_sequence;
    if (!SetProof(response, session.Credential().Value(), r)) {
        if (r.operation == JoinOperation::Resume) session.AbortResume(r.request_id, now);
        OPENSSL_cleanse(inserted->second.server_nonce.data(), inserted->second.server_nonce.size());
        transactions_.erase(inserted);
        return response.result = JoinResult::Rejected;
    }
    inserted->second.last_response = response;
    return response.result;
}

JoinResult JoinCoordinator::Confirm(SessionState& session, const JoinRequest& r, TimePoint now,
                                    JoinResponse& response) noexcept {
    response = JoinResponse{};
    Prune(now);
    auto it = transactions_.find(r.request_id);
    if (it == transactions_.end()) return response.result = JoinResult::NotFound;
    Transaction& tx = it->second;
    if (!Valid(r) || r.phase != JoinPhase::Confirm || !SameRequest(tx.request, r) ||
        r.server_nonce != tx.server_nonce || r.session_id != session.Snapshot().id ||
        r.client_session_id != session.Snapshot().client_session_id ||
        r.authenticated_identity != session.AuthenticatedIdentity() ||
        !session.Credential().IsSet() || !VerifyJoinProof(session.Credential().Value(), r))
        return response.result = JoinResult::Rejected;
    if (tx.phase == Phase::Committed) { response = tx.final_response; return response.result; }
    const bool duplicate = tx.phase == Phase::Confirmed;
    response.result = duplicate ? JoinResult::Duplicate : JoinResult::Accepted;
    response.server_nonce = tx.server_nonce;
    response.generation = r.generation;
    response.server_committed_operation_sequence = tx.server_committed_operation_sequence;
    if (!SetProof(response, session.Credential().Value(), r))
        return response.result = JoinResult::Rejected;
    tx.phase = Phase::Confirmed;
    tx.last_response = response;
    return response.result;
}

JoinResult JoinCoordinator::Commit(SessionState& session, const JoinRequest& r,
                                   std::uint64_t conn_gen, TimePoint now,
                                   JoinResponse& response) noexcept {
    response = JoinResponse{};
    Prune(now);
    auto it = transactions_.find(r.request_id);
    if (it == transactions_.end()) return response.result = JoinResult::NotFound;
    Transaction& tx = it->second;
    if (!Valid(r) || r.phase != JoinPhase::Commit || !SameRequest(tx.request, r) ||
        r.server_nonce != tx.server_nonce || r.session_id != session.Snapshot().id ||
        r.client_session_id != session.Snapshot().client_session_id ||
        r.authenticated_identity != session.AuthenticatedIdentity() ||
        !session.Credential().IsSet() || !VerifyJoinProof(session.Credential().Value(), r))
        return response.result = JoinResult::Rejected;
    if (tx.phase == Phase::Committed) { response = tx.final_response; return response.result; }
    if (tx.phase != Phase::Confirmed) return response.result = JoinResult::Rejected;
    Secret32 commit_proof{};
    if (!ComputeJoinResponseProof(session.Credential().Value(), r,
        tx.server_committed_operation_sequence, commit_proof))
        return response.result = JoinResult::Rejected;
    bool committed = false;
    if (r.operation == JoinOperation::Resume) {
        committed = session.CommitResume(r.request_id, r.carrier_id, conn_gen, now);
    } else if (r.generation == session.Snapshot().generation) {
        committed = session.OnCarrierJoined(r.carrier_id, r.generation, conn_gen, now);
    }
    if (!committed) {
        OPENSSL_cleanse(commit_proof.data(), commit_proof.size());
        if (r.operation == JoinOperation::Resume) session.AbortResume(r.request_id, now);
        return response.result = JoinResult::Rejected;
    }
    tx.phase = Phase::Committed;
    tx.final_response.result = JoinResult::Committed;
    tx.final_response.generation = session.Snapshot().generation;
    tx.final_response.server_nonce = tx.server_nonce;
    tx.final_response.server_committed_operation_sequence = tx.server_committed_operation_sequence;
    tx.final_response.proof = commit_proof;
    OPENSSL_cleanse(commit_proof.data(), commit_proof.size());
    response = tx.final_response;
    return response.result;
}

JoinResult JoinCoordinator::Query(const std::string& identity, const std::string& request_id,
                                  TimePoint now, JoinResponse& response) noexcept {
    response = JoinResponse{};
    Prune(now);
    auto it = transactions_.find(request_id);
    if (it == transactions_.end()) return response.result = JoinResult::NotFound;
    if (identity != it->second.request.authenticated_identity) return response.result = JoinResult::Rejected;
    if (it->second.phase == Phase::Committed) { response = it->second.final_response; return response.result; }
    response = it->second.last_response;
    response.result = it->second.phase == Phase::Confirmed ? JoinResult::Accepted : JoinResult::Duplicate;
    return response.result;
}

void JoinCoordinator::Cancel(const std::string& request_id) noexcept {
    auto it = transactions_.find(request_id);
    if (it != transactions_.end() && it->second.phase != Phase::Committed) {
        OPENSSL_cleanse(it->second.server_nonce.data(), it->second.server_nonce.size());
        transactions_.erase(it);
    }
}

} // namespace ppp::app::multipath
