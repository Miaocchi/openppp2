#include <ppp/app/multipath/WireProtocol.h>

#include <openssl/crypto.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace ppp::app::multipath::wire {
namespace {

constexpr std::uint8_t Domain[] = {
    'o','p','e','n','p','p','p','2','-','m','p','-','w','i','r','e','-','v','1',0
};

class Writer final {
public:
    bool U8(std::uint8_t value) noexcept { return Put(value); }
    bool U16(std::size_t value) noexcept {
        return value <= std::numeric_limits<std::uint16_t>::max() &&
            Put(static_cast<std::uint8_t>(value >> 8)) && Put(static_cast<std::uint8_t>(value));
    }
    bool U32(std::uint32_t value) noexcept {
        for (int i = 3; i >= 0; --i) if (!Put(static_cast<std::uint8_t>(value >> (i * 8)))) return false;
        return true;
    }
    bool U64(std::uint64_t value) noexcept {
        for (int i = 7; i >= 0; --i) if (!Put(static_cast<std::uint8_t>(value >> (i * 8)))) return false;
        return true;
    }
    bool Bytes(const std::uint8_t* value, std::size_t size) noexcept {
        if (size == 0) return true;
        if (value == nullptr || size > MaxPayloadBytes - data_.size()) return false;
        try { data_.insert(data_.end(), value, value + size); return true; }
        catch (...) { return false; }
    }
    template<std::size_t N> bool Bytes(const std::array<std::uint8_t, N>& value) noexcept {
        return Bytes(value.data(), value.size());
    }
    bool String(const std::string& value, std::size_t maximum) noexcept {
        return !value.empty() && value.size() <= maximum && U16(value.size()) &&
            Bytes(reinterpret_cast<const std::uint8_t*>(value.data()), value.size());
    }
    bool Finish(std::vector<std::uint8_t>& output) noexcept {
        if (data_.size() > MaxPayloadBytes) return false;
        output.swap(data_);
        return true;
    }
private:
    bool Put(std::uint8_t value) noexcept {
        if (data_.size() >= MaxPayloadBytes) return false;
        try { data_.push_back(value); return true; } catch (...) { return false; }
    }
    std::vector<std::uint8_t> data_;
};

class Reader final {
public:
    Reader(const std::uint8_t* data, std::size_t size) noexcept : data_(data), size_(size) {}
    bool U8(std::uint8_t& value) noexcept {
        if (position_ >= size_) return false;
        value = data_[position_++]; return true;
    }
    bool U16(std::uint16_t& value) noexcept {
        std::uint8_t a, b;
        if (!U8(a) || !U8(b)) return false;
        value = static_cast<std::uint16_t>((static_cast<std::uint16_t>(a) << 8) | b); return true;
    }
    bool U32(std::uint32_t& value) noexcept {
        value = 0;
        for (int i = 0; i < 4; ++i) { std::uint8_t b; if (!U8(b)) return false; value = (value << 8) | b; }
        return true;
    }
    bool U64(std::uint64_t& value) noexcept {
        value = 0;
        for (int i = 0; i < 8; ++i) { std::uint8_t b; if (!U8(b)) return false; value = (value << 8) | b; }
        return true;
    }
    bool Bytes(std::uint8_t* output, std::size_t length) noexcept {
        if (length == 0) return true;
        if (length > size_ - position_) return false;
        std::memcpy(output, data_ + position_, length); position_ += length; return true;
    }
    template<std::size_t N> bool Bytes(std::array<std::uint8_t, N>& output) noexcept {
        return Bytes(output.data(), output.size());
    }
    bool String(std::string& value, std::size_t maximum) noexcept {
        std::uint16_t length;
        if (!U16(length) || length == 0 || length > maximum || length > size_ - position_) return false;
        try { value.assign(reinterpret_cast<const char*>(data_ + position_), length); }
        catch (...) { return false; }
        position_ += length; return true;
    }
    bool StringAllowEmpty(std::string& value, std::size_t maximum) noexcept {
        std::uint16_t length;
        if (!U16(length) || length > maximum || length > size_ - position_) return false;
        try { value.assign(reinterpret_cast<const char*>(data_ + position_), length); }
        catch (...) { return false; }
        position_ += length; return true;
    }
    bool Done() const noexcept { return position_ == size_; }
private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t position_ = 0;
};

bool Begin(Writer& writer) noexcept { return writer.Bytes(Domain, sizeof(Domain)) && writer.U8(Version); }
bool Begin(Reader& reader) noexcept {
    std::array<std::uint8_t, sizeof(Domain)> domain{};
    std::uint8_t version = 0;
    return reader.Bytes(domain) && std::equal(domain.begin(), domain.end(), std::begin(Domain)) &&
        reader.U8(version) && version == Version;
}
bool Valid(const CreateRequest& value) noexcept {
    return !value.authenticated_identity.empty() && value.authenticated_identity.size() <= MaxIdentityBytes &&
        !value.request_id.empty() && value.request_id.size() <= MaxRequestIdBytes &&
        !value.carrier_id.empty() && value.carrier_id.size() <= MaxCarrierIdBytes;
}
bool Valid(const CreateResponse& value) noexcept {
    return value.result >= CreateResult::Accepted && value.result <= CreateResult::Conflict &&
        value.grace_ms <= MaxGraceMilliseconds &&
        (value.result != CreateResult::Accepted && value.result != CreateResult::Duplicate ||
            value.generation != 0 && value.grace_ms != 0);
}
bool Valid(const QueryRequest& value) noexcept {
    return (value.operation == QueryOperation::Create || value.operation == QueryOperation::Join) &&
        !value.authenticated_identity.empty() && value.authenticated_identity.size() <= MaxIdentityBytes &&
        !value.request_id.empty() && value.request_id.size() <= MaxRequestIdBytes &&
        !value.carrier_id.empty() && value.carrier_id.size() <= MaxCarrierIdBytes;
}

} // namespace

bool EncodeCreateRequest(const CreateRequest& v, std::vector<std::uint8_t>& out) noexcept {
    Writer w;
    return Valid(v) && Begin(w) && w.String(v.authenticated_identity, MaxIdentityBytes) &&
        w.String(v.request_id, MaxRequestIdBytes) && w.String(v.carrier_id, MaxCarrierIdBytes) &&
        w.Bytes(v.client_session_id) && w.Bytes(v.client_nonce) && w.Bytes(v.candidate_binding) &&
        w.U8(v.failover ? 1 : 0) && w.Finish(out);
}
bool DecodeCreateRequest(const std::uint8_t* data, std::size_t size, CreateRequest& v) noexcept {
    if (data == nullptr || size > MaxPayloadBytes) return false;
    Reader r(data, size); CreateRequest x;
    if (!Begin(r) || !r.String(x.authenticated_identity, MaxIdentityBytes) || !r.String(x.request_id, MaxRequestIdBytes) ||
        !r.String(x.carrier_id, MaxCarrierIdBytes) || !r.Bytes(x.client_session_id) || !r.Bytes(x.client_nonce) ||
        !r.Bytes(x.candidate_binding)) return false;
    std::uint8_t failover = 0;
    if (!r.U8(failover) || failover > 1 || !r.Done() || !Valid(x)) return false;
    x.failover = failover != 0;
    v = std::move(x); return true;
}
bool EncodeCreateResponse(const CreateResponse& v, std::vector<std::uint8_t>& out) noexcept {
    Writer w;
    return Valid(v) && Begin(w) && w.U8(static_cast<std::uint8_t>(v.result)) && w.Bytes(v.client_session_id) &&
        w.Bytes(v.session_id) && w.U64(v.generation) && w.Bytes(v.credential) && w.U32(v.grace_ms) &&
        w.U8(v.failover ? 1 : 0) && w.Finish(out);
}
bool DecodeCreateResponse(const std::uint8_t* data, std::size_t size, CreateResponse& v) noexcept {
    if (data == nullptr || size > MaxPayloadBytes) return false;
    Reader r(data, size); CreateResponse x; std::uint8_t result = 0;
    if (!Begin(r) || !r.U8(result) || result < static_cast<std::uint8_t>(CreateResult::Accepted) ||
        result > static_cast<std::uint8_t>(CreateResult::Conflict)) return false;
    x.result = static_cast<CreateResult>(result);
    if (!r.Bytes(x.client_session_id) || !r.Bytes(x.session_id) || !r.U64(x.generation) ||
        !r.Bytes(x.credential) || !r.U32(x.grace_ms)) return false;
    std::uint8_t failover = 0;
    if (!r.U8(failover) || failover > 1 || !r.Done() || !Valid(x)) return false;
    x.failover = failover != 0;
    v = x; return true;
}
bool EncodeJoinRequest(const JoinRequest& v, std::vector<std::uint8_t>& out) noexcept {
    Writer w;
    if ((v.operation != JoinOperation::Join && v.operation != JoinOperation::Resume) ||
        (v.phase != JoinPhase::Reserve && v.phase != JoinPhase::Confirm && v.phase != JoinPhase::Commit) ||
        v.authenticated_identity.empty() || v.authenticated_identity.size() > MaxIdentityBytes ||
        v.request_id.empty() || v.request_id.size() > MaxRequestIdBytes ||
        v.carrier_id.empty() || v.carrier_id.size() > MaxCarrierIdBytes) return false;
    return Begin(w) && w.U8(static_cast<std::uint8_t>(v.operation)) && w.U8(static_cast<std::uint8_t>(v.phase)) &&
        w.String(v.authenticated_identity, MaxIdentityBytes) && w.String(v.request_id, MaxRequestIdBytes) &&
        w.String(v.carrier_id, MaxCarrierIdBytes) && w.Bytes(v.client_session_id) && w.Bytes(v.session_id) &&
        w.U64(v.generation) && w.U64(v.peer_committed_operation_sequence) &&
        w.Bytes(v.client_nonce) && w.Bytes(v.server_nonce) &&
        w.Bytes(v.candidate_binding) && w.Bytes(v.proof) && w.Finish(out);
}
bool DecodeJoinRequest(const std::uint8_t* data, std::size_t size, JoinRequest& v) noexcept {
    if (data == nullptr || size > MaxPayloadBytes) return false;
    Reader r(data, size); JoinRequest x; std::uint8_t operation = 0, phase = 0;
    if (!Begin(r) || !r.U8(operation) || !r.U8(phase) ||
        (operation != static_cast<std::uint8_t>(JoinOperation::Join) && operation != static_cast<std::uint8_t>(JoinOperation::Resume)) ||
        (phase < static_cast<std::uint8_t>(JoinPhase::Reserve) || phase > static_cast<std::uint8_t>(JoinPhase::Commit))) return false;
    x.operation = static_cast<JoinOperation>(operation); x.phase = static_cast<JoinPhase>(phase);
    if (!r.String(x.authenticated_identity, MaxIdentityBytes) || !r.String(x.request_id, MaxRequestIdBytes) ||
        !r.String(x.carrier_id, MaxCarrierIdBytes) || !r.Bytes(x.client_session_id) || !r.Bytes(x.session_id) ||
        !r.U64(x.generation) || !r.U64(x.peer_committed_operation_sequence) ||
        !r.Bytes(x.client_nonce) || !r.Bytes(x.server_nonce) ||
        !r.Bytes(x.candidate_binding) || !r.Bytes(x.proof) || !r.Done()) return false;
    v = std::move(x); return true;
}
bool EncodeJoinResponse(const JoinResponse& v, std::vector<std::uint8_t>& out) noexcept {
    Writer w;
    if (v.result > JoinResult::Capacity) return false;
    return Begin(w) && w.U8(static_cast<std::uint8_t>(v.result)) && w.Bytes(v.server_nonce) &&
        w.Bytes(v.proof) && w.U64(v.generation) &&
        w.U64(v.server_committed_operation_sequence) && w.Finish(out);
}
bool DecodeJoinResponse(const std::uint8_t* data, std::size_t size, JoinResponse& v) noexcept {
    if (data == nullptr || size > MaxPayloadBytes) return false;
    Reader r(data, size); JoinResponse x; std::uint8_t result = 0;
    if (!Begin(r) || !r.U8(result) || result > static_cast<std::uint8_t>(JoinResult::Capacity) ||
        !r.Bytes(x.server_nonce) || !r.Bytes(x.proof) || !r.U64(x.generation) ||
        !r.U64(x.server_committed_operation_sequence) || !r.Done()) return false;
    x.result = static_cast<JoinResult>(result); v = x; return true;
}
bool EncodeQueryRequest(const QueryRequest& v, std::vector<std::uint8_t>& out) noexcept {
    Writer w;
    return Valid(v) && Begin(w) && w.U8(static_cast<std::uint8_t>(v.operation)) &&
        w.String(v.authenticated_identity, MaxIdentityBytes) && w.String(v.request_id, MaxRequestIdBytes) &&
        w.String(v.carrier_id, MaxCarrierIdBytes) && w.Bytes(v.client_session_id) &&
        w.Bytes(v.client_nonce) && w.Bytes(v.candidate_binding) && w.Finish(out);
}
bool DecodeQueryRequest(const std::uint8_t* data, std::size_t size, QueryRequest& v) noexcept {
    if (data == nullptr || size > MaxPayloadBytes) return false;
    Reader r(data, size); QueryRequest x; std::uint8_t operation = 0;
    if (!Begin(r) || !r.U8(operation) ||
        (operation != static_cast<std::uint8_t>(QueryOperation::Create) && operation != static_cast<std::uint8_t>(QueryOperation::Join))) return false;
    x.operation = static_cast<QueryOperation>(operation);
    if (!r.String(x.authenticated_identity, MaxIdentityBytes) || !r.String(x.request_id, MaxRequestIdBytes) ||
        !r.String(x.carrier_id, MaxCarrierIdBytes) || !r.Bytes(x.client_session_id) ||
        !r.Bytes(x.client_nonce) || !r.Bytes(x.candidate_binding) || !r.Done() || !Valid(x)) return false;
    v = std::move(x); return true;
}
bool EncodeQueryResponse(const QueryResponse& v, std::vector<std::uint8_t>& out) noexcept {
    Writer w;
    if (!Begin(w) || !w.U8(static_cast<std::uint8_t>(v.operation))) return false;
    if (v.operation == QueryOperation::Create) {
        if (!Valid(v.create)) return false;
        return w.U8(0) && w.U8(static_cast<std::uint8_t>(v.create.result)) && w.Bytes(v.create.client_session_id) &&
            w.Bytes(v.create.session_id) && w.U64(v.create.generation) && w.Bytes(v.create.credential) &&
            w.U32(v.create.grace_ms) && w.U8(v.create.failover ? 1 : 0) && w.Finish(out);
    }
    if (v.operation == QueryOperation::Join) {
        if (v.join.result > JoinResult::Capacity) return false;
        return w.U8(0) && w.U8(static_cast<std::uint8_t>(v.join.result)) && w.Bytes(v.join.server_nonce) &&
            w.Bytes(v.join.proof) && w.U64(v.join.generation) &&
            w.U64(v.join.server_committed_operation_sequence) && w.Finish(out);
    }
    return false;
}
bool DecodeQueryResponse(const std::uint8_t* data, std::size_t size, QueryResponse& v) noexcept {
    if (data == nullptr || size > MaxPayloadBytes) return false;
    Reader r(data, size); QueryResponse x; std::uint8_t operation = 0, reserved = 0, result = 0;
    if (!Begin(r) || !r.U8(operation) || !r.U8(reserved) || reserved != 0 ||
        (operation != static_cast<std::uint8_t>(QueryOperation::Create) && operation != static_cast<std::uint8_t>(QueryOperation::Join)) ||
        !r.U8(result)) return false;
    x.operation = static_cast<QueryOperation>(operation);
    if (x.operation == QueryOperation::Create) {
        if (result < static_cast<std::uint8_t>(CreateResult::Accepted) || result > static_cast<std::uint8_t>(CreateResult::Conflict)) return false;
        x.create.result = static_cast<CreateResult>(result);
        if (!r.Bytes(x.create.client_session_id) || !r.Bytes(x.create.session_id) || !r.U64(x.create.generation) ||
            !r.Bytes(x.create.credential) || !r.U32(x.create.grace_ms)) return false;
        std::uint8_t failover = 0;
        if (!r.U8(failover) || failover > 1 || !r.Done()) return false;
        x.create.failover = failover != 0;
        if (!Valid(x.create)) return false;
    } else {
        if (result > static_cast<std::uint8_t>(JoinResult::Capacity)) return false;
        x.join.result = static_cast<JoinResult>(result);
        if (!r.Bytes(x.join.server_nonce) || !r.Bytes(x.join.proof) || !r.U64(x.join.generation) ||
            !r.U64(x.join.server_committed_operation_sequence) || !r.Done()) return false;
    }
    v = x; return true;
}

bool EncodeControlMessage(const ControlMessage& v, std::vector<std::uint8_t>& out) noexcept {
    const bool valid_ack = v.acknowledgement_only && v.sequence == 0 && v.operation_sequence == 0 &&
        v.operation_id.empty() && v.payload.empty();
    const bool valid_operation = !v.acknowledgement_only && v.sequence != 0 &&
        v.operation_sequence != 0 && !v.operation_id.empty() &&
        v.operation_id.size() <= MaxControlOperationId && v.payload.size() <= MaxControlPayload;
    if (v.session_generation == 0 || (!valid_ack && !valid_operation)) return false;
    Writer w;
    return Begin(w) && w.U64(v.session_generation) && w.U8(v.acknowledgement_only ? 1 : 0) &&
        w.U64(v.sequence) && w.U64(v.acknowledged) && w.U64(v.acknowledged_operation_sequence) && w.U64(v.operation_sequence) &&
        (v.operation_id.empty() ? w.U16(0) : w.String(v.operation_id, MaxControlOperationId)) &&
        w.U16(v.payload.size()) &&
        w.Bytes(v.payload.data(), v.payload.size()) && w.Finish(out);
}

bool DecodeControlMessage(const std::uint8_t* data, std::size_t size, ControlMessage& v) noexcept {
    if (data == nullptr || size > MaxPayloadBytes) return false;
    Reader r(data, size); ControlMessage x; std::uint8_t acknowledgement_only = 0;
    std::uint16_t payload_size = 0;
    if (!Begin(r) || !r.U64(x.session_generation) || x.session_generation == 0 ||
        !r.U8(acknowledgement_only) || acknowledgement_only > 1 ||
        !r.U64(x.sequence) || !r.U64(x.acknowledged) || !r.U64(x.acknowledged_operation_sequence) || !r.U64(x.operation_sequence) ||
        !r.StringAllowEmpty(x.operation_id, MaxControlOperationId) || !r.U16(payload_size) ||
        payload_size > MaxControlPayload || payload_size > size) return false;
    x.acknowledgement_only = acknowledgement_only != 0;
    try { x.payload.resize(payload_size); } catch (...) { return false; }
    if (!r.Bytes(x.payload.data(), x.payload.size()) || !r.Done()) return false;
    const bool valid_ack = x.acknowledgement_only && x.sequence == 0 && x.operation_sequence == 0 &&
        x.operation_id.empty() && x.payload.empty();
    const bool valid_operation = !x.acknowledgement_only && x.sequence != 0 &&
        x.operation_sequence != 0 && !x.operation_id.empty();
    if (!valid_ack && !valid_operation) return false;
    v = std::move(x); return true;
}

bool EncodeControlPayload(const ControlPayload& v, std::vector<std::uint8_t>& out) noexcept {
    if (v.opcode != ControlOpcode::Ping && v.opcode != ControlOpcode::Pong) return false;
    Writer w;
    return w.U8(static_cast<std::uint8_t>(v.opcode)) && w.Bytes(v.nonce) && w.Finish(out);
}

bool DecodeControlPayload(const std::uint8_t* data, std::size_t size, ControlPayload& v) noexcept {
    if (data == nullptr || size != 9) return false;
    Reader r(data, size); ControlPayload x; std::uint8_t opcode = 0;
    if (!r.U8(opcode) || (opcode != static_cast<std::uint8_t>(ControlOpcode::Ping) &&
        opcode != static_cast<std::uint8_t>(ControlOpcode::Pong)) || !r.Bytes(x.nonce) || !r.Done()) return false;
    x.opcode = static_cast<ControlOpcode>(opcode);
    v = x;
    return true;
}

std::string MakeControlOperationId(ControlOpcode opcode, std::uint64_t generation,
    std::uint64_t operation_sequence) {
    const char* prefix = opcode == ControlOpcode::Ping ? "ping/" : "pong/";
    return std::string(prefix) + std::to_string(generation) + "/" + std::to_string(operation_sequence);
}

bool BuildCandidateBindingContext(const SessionId& client_session_id, const std::string& request_id,
    const std::string& carrier_id, const Secret32& client_nonce, std::vector<std::uint8_t>& output) noexcept {
    Writer w;
    return w.Bytes(Domain, sizeof(Domain)) && w.U8(Version) && w.Bytes(client_session_id) &&
        w.String(request_id, MaxRequestIdBytes) && w.String(carrier_id, MaxCarrierIdBytes) &&
        w.Bytes(client_nonce) && w.Finish(output);
}

SessionId ClientSessionIdFromInt128(const ppp::Int128& value) noexcept {
    std::uint64_t high = 0, low = 0;
#if defined(_PPP_INT128)
    high = static_cast<std::uint64_t>(value.hi);
    low = value.lo;
#else
    const __uint128_t bits = static_cast<__uint128_t>(value);
    high = static_cast<std::uint64_t>(bits >> 64);
    low = static_cast<std::uint64_t>(bits);
#endif
    SessionId result{};
    for (int i = 0; i < 8; ++i) {
        result[i] = static_cast<std::uint8_t>(high >> ((7 - i) * 8));
        result[8 + i] = static_cast<std::uint8_t>(low >> ((7 - i) * 8));
    }
    return result;
}

ppp::Int128 ClientSessionIdToInt128(const SessionId& value) noexcept {
    std::uint64_t high = 0, low = 0;
    for (int i = 0; i < 8; ++i) {
        high = (high << 8) | value[i];
        low = (low << 8) | value[8 + i];
    }
    return ppp::MAKE_OWORD(low, high);
}

void ClearSensitivePayload(std::vector<std::uint8_t>& payload) noexcept {
    if (!payload.empty()) OPENSSL_cleanse(payload.data(), payload.size());
    payload.clear();
}

} // namespace ppp::app::multipath::wire
