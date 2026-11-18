#pragma once

#include <ppp/app/multipath/Join.h>
#include <ppp/app/multipath/Control.h>
#include <ppp/Int128.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ppp::app::multipath::wire {

inline constexpr std::uint8_t Version = 1;
inline constexpr std::size_t MaxPayloadBytes = 8192;
inline constexpr std::size_t MaxGraceMilliseconds = 30000;

enum class RecordType : std::uint8_t {
    CreateRequest = 0x10,
    CreateResponse = 0x11,
    JoinRequest = 0x12,
    JoinResponse = 0x13,
    QueryRequest = 0x14,
    QueryResponse = 0x15,
    ReliableControl = 0x01,
};

enum class CreateResult : std::uint8_t {
    Accepted = 1,
    Duplicate = 2,
    Rejected = 3,
    Capacity = 4,
    Conflict = 5,
};

enum class QueryOperation : std::uint8_t { Create = 1, Join = 2 };
enum class ControlOpcode : std::uint8_t { Ping = 1, Pong = 2 };

struct ControlPayload final {
    ControlOpcode opcode = ControlOpcode::Ping;
    std::array<std::uint8_t, 8> nonce{};
};

struct CreateRequest final {
    std::string authenticated_identity;
    std::string request_id;
    std::string carrier_id;
    SessionId client_session_id{};
    Secret32 client_nonce{};
    Secret32 candidate_binding{};
    bool failover = false;
};

struct CreateResponse final {
    CreateResult result = CreateResult::Rejected;
    SessionId client_session_id{};
    SessionId session_id{};
    std::uint64_t generation = 0;
    Secret32 credential{};
    std::uint32_t grace_ms = 0;
    bool failover = false;
};

struct QueryRequest final {
    QueryOperation operation = QueryOperation::Create;
    std::string authenticated_identity;
    std::string request_id;
    std::string carrier_id;
    SessionId client_session_id{};
    Secret32 client_nonce{};
    Secret32 candidate_binding{};
};

struct QueryResponse final {
    QueryOperation operation = QueryOperation::Create;
    CreateResponse create;
    JoinResponse join;
};

bool EncodeCreateRequest(const CreateRequest& value, std::vector<std::uint8_t>& output) noexcept;
bool DecodeCreateRequest(const std::uint8_t* data, std::size_t size, CreateRequest& value) noexcept;
bool EncodeCreateResponse(const CreateResponse& value, std::vector<std::uint8_t>& output) noexcept;
bool DecodeCreateResponse(const std::uint8_t* data, std::size_t size, CreateResponse& value) noexcept;
bool EncodeJoinRequest(const JoinRequest& value, std::vector<std::uint8_t>& output) noexcept;
bool DecodeJoinRequest(const std::uint8_t* data, std::size_t size, JoinRequest& value) noexcept;
bool EncodeJoinResponse(const JoinResponse& value, std::vector<std::uint8_t>& output) noexcept;
bool DecodeJoinResponse(const std::uint8_t* data, std::size_t size, JoinResponse& value) noexcept;
bool EncodeQueryRequest(const QueryRequest& value, std::vector<std::uint8_t>& output) noexcept;
bool DecodeQueryRequest(const std::uint8_t* data, std::size_t size, QueryRequest& value) noexcept;
bool EncodeQueryResponse(const QueryResponse& value, std::vector<std::uint8_t>& output) noexcept;
bool DecodeQueryResponse(const std::uint8_t* data, std::size_t size, QueryResponse& value) noexcept;
bool EncodeControlMessage(const ControlMessage& value, std::vector<std::uint8_t>& output) noexcept;
bool DecodeControlMessage(const std::uint8_t* data, std::size_t size, ControlMessage& value) noexcept;
bool EncodeControlPayload(const ControlPayload& value, std::vector<std::uint8_t>& output) noexcept;
bool DecodeControlPayload(const std::uint8_t* data, std::size_t size, ControlPayload& value) noexcept;
std::string MakeControlOperationId(ControlOpcode opcode, std::uint64_t generation,
    std::uint64_t operation_sequence);

bool BuildCandidateBindingContext(const SessionId& client_session_id,
    const std::string& request_id, const std::string& carrier_id,
    const Secret32& client_nonce, std::vector<std::uint8_t>& output) noexcept;
SessionId ClientSessionIdFromInt128(const ppp::Int128& value) noexcept;
ppp::Int128 ClientSessionIdToInt128(const SessionId& value) noexcept;
void ClearSensitivePayload(std::vector<std::uint8_t>& payload) noexcept;

} // namespace ppp::app::multipath::wire
