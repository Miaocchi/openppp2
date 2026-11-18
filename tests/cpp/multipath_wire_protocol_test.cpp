#define BOOST_TEST_MODULE multipath_wire_protocol_test
#include <boost/test/included/unit_test.hpp>

#include <ppp/app/multipath/WireProtocol.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace mp = ppp::app::multipath;
namespace wire = ppp::app::multipath::wire;

namespace {
template <std::size_t N>
std::array<std::uint8_t, N> Filled(std::uint8_t seed) {
    std::array<std::uint8_t, N> result{};
    for (std::size_t i = 0; i < N; ++i) result[i] = static_cast<std::uint8_t>(seed + i);
    return result;
}

template <typename Decode>
void RejectMalformedEnvelope(const std::vector<std::uint8_t>& bytes, Decode decode) {
    for (std::size_t size = 0; size < bytes.size(); ++size) {
        const auto* data = size == 0 ? nullptr : bytes.data();
        BOOST_CHECK(!decode(data, size));
    }
    auto trailing = bytes;
    trailing.push_back(0);
    BOOST_CHECK(!decode(trailing.data(), trailing.size()));

    auto wrong_domain = bytes;
    wrong_domain[0] ^= 0x80;
    BOOST_CHECK(!decode(wrong_domain.data(), wrong_domain.size()));
    auto wrong_version = bytes;
    wrong_version[20] = static_cast<std::uint8_t>(wire::Version + 1);
    BOOST_CHECK(!decode(wrong_version.data(), wrong_version.size()));

    std::vector<std::uint8_t> oversized(wire::MaxPayloadBytes + 1, 0);
    BOOST_CHECK(!decode(oversized.data(), oversized.size()));
}

template <typename Value, typename Encode, typename Decode, typename Equal>
std::vector<std::uint8_t> RoundTrip(const Value& value, Encode encode, Decode decode,
                                    Equal equal) {
    std::vector<std::uint8_t> bytes;
    BOOST_REQUIRE(encode(value, bytes));
    Value decoded;
    BOOST_REQUIRE(decode(bytes.data(), bytes.size(), decoded));
    BOOST_CHECK(equal(value, decoded));
    RejectMalformedEnvelope(bytes, [&](const std::uint8_t* data, std::size_t size) {
        Value output;
        return decode(data, size, output);
    });
    return bytes;
}

wire::CreateRequest CreateRequestValue() {
    wire::CreateRequest value;
    value.authenticated_identity = "principal-1";
    value.request_id = "create-001";
    value.carrier_id = "edge-primary";
    value.client_session_id = Filled<16>(4);
    value.client_nonce = Filled<32>(29);
    value.candidate_binding = Filled<32>(71);
    value.failover = true;
    return value;
}

wire::CreateResponse CreateResponseValue() {
    wire::CreateResponse value;
    value.result = wire::CreateResult::Accepted;
    value.client_session_id = Filled<16>(2);
    value.session_id = Filled<16>(22);
    value.generation = 7;
    value.credential = Filled<32>(42);
    value.grace_ms = 30000;
    value.failover = true;
    return value;
}

mp::JoinRequest JoinRequestValue() {
    mp::JoinRequest value;
    value.operation = mp::JoinOperation::Resume;
    value.phase = mp::JoinPhase::Commit;
    value.authenticated_identity = "principal-7";
    value.request_id = "resume-request-1";
    value.carrier_id = "cdn-b";
    value.client_session_id = Filled<16>(1);
    value.session_id = Filled<16>(17);
    value.generation = 0x0102030405060708ull;
    value.peer_committed_operation_sequence = 0x1112131415161718ull;
    value.client_nonce = Filled<32>(33);
    value.server_nonce = Filled<32>(65);
    value.candidate_binding = Filled<32>(97);
    value.proof = Filled<32>(129);
    return value;
}

mp::JoinResponse JoinResponseValue() {
    mp::JoinResponse value;
    value.result = mp::JoinResult::Committed;
    value.server_nonce = Filled<32>(5);
    value.proof = Filled<32>(37);
    value.generation = 9;
    value.server_committed_operation_sequence = 0x2122232425262728ull;
    return value;
}

wire::QueryRequest QueryRequestValue() {
    wire::QueryRequest value;
    value.operation = wire::QueryOperation::Join;
    value.authenticated_identity = "principal-7";
    value.request_id = "query-request-9";
    value.carrier_id = "wss-cdn-a";
    value.client_session_id = Filled<16>(3);
    value.client_nonce = Filled<32>(17);
    value.candidate_binding = Filled<32>(49);
    return value;
}

bool Same(const wire::CreateRequest& a, const wire::CreateRequest& b) {
    return a.authenticated_identity == b.authenticated_identity && a.request_id == b.request_id &&
        a.carrier_id == b.carrier_id && a.client_session_id == b.client_session_id &&
        a.client_nonce == b.client_nonce && a.candidate_binding == b.candidate_binding &&
        a.failover == b.failover;
}
bool Same(const wire::CreateResponse& a, const wire::CreateResponse& b) {
    return a.result == b.result && a.client_session_id == b.client_session_id &&
        a.session_id == b.session_id && a.generation == b.generation &&
        a.credential == b.credential && a.grace_ms == b.grace_ms && a.failover == b.failover;
}
bool Same(const mp::JoinRequest& a, const mp::JoinRequest& b) {
    return a.operation == b.operation && a.phase == b.phase &&
        a.authenticated_identity == b.authenticated_identity && a.request_id == b.request_id &&
        a.carrier_id == b.carrier_id && a.client_session_id == b.client_session_id &&
        a.session_id == b.session_id && a.generation == b.generation &&
        a.peer_committed_operation_sequence == b.peer_committed_operation_sequence &&
        a.client_nonce == b.client_nonce && a.server_nonce == b.server_nonce &&
        a.candidate_binding == b.candidate_binding && a.proof == b.proof;
}
bool Same(const mp::JoinResponse& a, const mp::JoinResponse& b) {
    return a.result == b.result && a.server_nonce == b.server_nonce &&
        a.proof == b.proof && a.generation == b.generation &&
        a.server_committed_operation_sequence == b.server_committed_operation_sequence;
}
bool Same(const wire::QueryRequest& a, const wire::QueryRequest& b) {
    return a.operation == b.operation && a.authenticated_identity == b.authenticated_identity &&
        a.request_id == b.request_id && a.carrier_id == b.carrier_id &&
        a.client_session_id == b.client_session_id && a.client_nonce == b.client_nonce &&
        a.candidate_binding == b.candidate_binding;
}
bool Same(const wire::QueryResponse& a, const wire::QueryResponse& b) {
    return a.operation == b.operation && (a.operation == wire::QueryOperation::Create
        ? Same(a.create, b.create) : Same(a.join, b.join));
}
bool Same(const mp::ControlMessage& a, const mp::ControlMessage& b) {
    return a.session_generation == b.session_generation && a.acknowledgement_only == b.acknowledgement_only &&
        a.sequence == b.sequence && a.acknowledged == b.acknowledged &&
        a.operation_sequence == b.operation_sequence &&
        a.acknowledged_operation_sequence == b.acknowledged_operation_sequence &&
        a.operation_id == b.operation_id &&
        a.payload == b.payload;
}
}

BOOST_AUTO_TEST_CASE(all_create_join_and_query_messages_round_trip_and_reject_truncation) {
    const auto create_request = CreateRequestValue();
    const auto create_request_bytes = RoundTrip(create_request, wire::EncodeCreateRequest,
        wire::DecodeCreateRequest, [](const auto& a, const auto& b) { return Same(a, b); });

    const auto create_response = CreateResponseValue();
    const auto create_response_bytes = RoundTrip(create_response, wire::EncodeCreateResponse,
        wire::DecodeCreateResponse, [](const auto& a, const auto& b) { return Same(a, b); });

    const auto join_request = JoinRequestValue();
    const auto join_request_bytes = RoundTrip(join_request, wire::EncodeJoinRequest,
        wire::DecodeJoinRequest, [](const auto& a, const auto& b) { return Same(a, b); });

    const auto join_response = JoinResponseValue();
    const auto join_response_bytes = RoundTrip(join_response, wire::EncodeJoinResponse,
        wire::DecodeJoinResponse, [](const auto& a, const auto& b) { return Same(a, b); });

    const auto query_request = QueryRequestValue();
    const auto query_request_bytes = RoundTrip(query_request, wire::EncodeQueryRequest,
        wire::DecodeQueryRequest, [](const auto& a, const auto& b) { return Same(a, b); });

    wire::QueryResponse query_create;
    query_create.operation = wire::QueryOperation::Create;
    query_create.create = create_response;
    const auto query_create_bytes = RoundTrip(query_create, wire::EncodeQueryResponse,
        wire::DecodeQueryResponse, [](const auto& a, const auto& b) { return Same(a, b); });

    wire::QueryResponse query_join;
    query_join.operation = wire::QueryOperation::Join;
    query_join.join = join_response;
    const auto query_join_bytes = RoundTrip(query_join, wire::EncodeQueryResponse,
        wire::DecodeQueryResponse, [](const auto& a, const auto& b) { return Same(a, b); });

    mp::ControlMessage control;
    control.session_generation = 22;
    control.sequence = 19;
    control.acknowledged = 13;
    control.operation_sequence = 17;
    control.acknowledged_operation_sequence = 11;
    control.operation_id = "operation-17";
    control.payload = {0, 1, 2, 0xff};
    const auto control_bytes = RoundTrip(control, wire::EncodeControlMessage,
        wire::DecodeControlMessage, [](const auto& a, const auto& b) { return Same(a, b); });

    mp::ControlMessage ack_only;
    ack_only.session_generation = 22;
    ack_only.acknowledged = 19;
    ack_only.acknowledged_operation_sequence = 17;
    ack_only.acknowledgement_only = true;
    const auto ack_bytes = RoundTrip(ack_only, wire::EncodeControlMessage,
        wire::DecodeControlMessage, [](const auto& a, const auto& b) { return Same(a, b); });

    BOOST_TEST(!create_request_bytes.empty());
    BOOST_TEST(!create_response_bytes.empty());
    BOOST_TEST(!join_request_bytes.empty());
    BOOST_TEST(!join_response_bytes.empty());
    BOOST_TEST(!query_request_bytes.empty());
    BOOST_TEST(!query_create_bytes.empty());
    BOOST_TEST(!query_join_bytes.empty());
    BOOST_TEST(!control_bytes.empty());
    BOOST_TEST(!ack_bytes.empty());
}

BOOST_AUTO_TEST_CASE(unknown_discriminants_and_impossible_responses_are_rejected) {
    auto create_response = CreateResponseValue();
    std::vector<std::uint8_t> bytes;
    BOOST_REQUIRE(wire::EncodeCreateResponse(create_response, bytes));
    bytes[21] = 0xff;
    wire::CreateResponse decoded_create;
    BOOST_TEST(!wire::DecodeCreateResponse(bytes.data(), bytes.size(), decoded_create));
    create_response.result = static_cast<wire::CreateResult>(255);
    BOOST_TEST(!wire::EncodeCreateResponse(create_response, bytes));

    auto join_request = JoinRequestValue();
    BOOST_REQUIRE(wire::EncodeJoinRequest(join_request, bytes));
    bytes[21] = 0xff;
    mp::JoinRequest decoded_join;
    BOOST_TEST(!wire::DecodeJoinRequest(bytes.data(), bytes.size(), decoded_join));
    BOOST_REQUIRE(wire::EncodeJoinRequest(join_request, bytes));
    bytes[22] = 0xff;
    BOOST_TEST(!wire::DecodeJoinRequest(bytes.data(), bytes.size(), decoded_join));

    auto join_response = JoinResponseValue();
    join_response.result = static_cast<mp::JoinResult>(255);
    BOOST_TEST(!wire::EncodeJoinResponse(join_response, bytes));
    join_response = JoinResponseValue();
    BOOST_REQUIRE(wire::EncodeJoinResponse(join_response, bytes));
    bytes[21] = 0xff;
    mp::JoinResponse decoded_response;
    BOOST_TEST(!wire::DecodeJoinResponse(bytes.data(), bytes.size(), decoded_response));

    auto query_request = QueryRequestValue();
    BOOST_REQUIRE(wire::EncodeQueryRequest(query_request, bytes));
    bytes[21] = 0xff;
    wire::QueryRequest decoded_query;
    BOOST_TEST(!wire::DecodeQueryRequest(bytes.data(), bytes.size(), decoded_query));

    wire::QueryResponse query_create;
    query_create.operation = wire::QueryOperation::Create;
    query_create.create = CreateResponseValue();
    query_create.create.generation = 0;
    BOOST_TEST(!wire::EncodeQueryResponse(query_create, bytes));
    query_create.create = CreateResponseValue();
    query_create.create.result = static_cast<wire::CreateResult>(255);
    BOOST_TEST(!wire::EncodeQueryResponse(query_create, bytes));

    wire::QueryResponse query_join;
    query_join.operation = wire::QueryOperation::Join;
    query_join.join = JoinResponseValue();
    query_join.join.result = static_cast<mp::JoinResult>(255);
    BOOST_TEST(!wire::EncodeQueryResponse(query_join, bytes));
    query_join.join = JoinResponseValue();
    BOOST_REQUIRE(wire::EncodeQueryResponse(query_join, bytes));
    bytes[23] = 0xff;
    wire::QueryResponse decoded_query_response;
    BOOST_TEST(!wire::DecodeQueryResponse(bytes.data(), bytes.size(), decoded_query_response));

    mp::ControlMessage control;
    control.session_generation = 1;
    control.sequence = 1;
    control.operation_sequence = 1;
    control.operation_id = "op";
    BOOST_REQUIRE(wire::EncodeControlMessage(control, bytes));
    bytes[29] = 2;
    mp::ControlMessage decoded_control;
    BOOST_TEST(!wire::DecodeControlMessage(bytes.data(), bytes.size(), decoded_control));
}

BOOST_AUTO_TEST_CASE(maximum_lengths_and_query_context_are_bound) {
    auto request = CreateRequestValue();
    request.authenticated_identity.assign(mp::MaxIdentityBytes, 'i');
    request.request_id.assign(mp::MaxRequestIdBytes, 'r');
    request.carrier_id.assign(mp::MaxCarrierIdBytes, 'c');
    std::vector<std::uint8_t> bytes;
    BOOST_REQUIRE(wire::EncodeCreateRequest(request, bytes));
    wire::CreateRequest decoded;
    BOOST_REQUIRE(wire::DecodeCreateRequest(bytes.data(), bytes.size(), decoded));
    BOOST_CHECK(Same(request, decoded));
    auto oversized_create = request;
    oversized_create.authenticated_identity.push_back('i');
    BOOST_TEST(!wire::EncodeCreateRequest(oversized_create, bytes));
    oversized_create = request;
    oversized_create.request_id.push_back('r');
    BOOST_TEST(!wire::EncodeCreateRequest(oversized_create, bytes));
    oversized_create = request;
    oversized_create.carrier_id.push_back('c');
    BOOST_TEST(!wire::EncodeCreateRequest(oversized_create, bytes));

    auto join = JoinRequestValue();
    join.authenticated_identity.assign(mp::MaxIdentityBytes, 'i');
    join.request_id.assign(mp::MaxRequestIdBytes, 'r');
    join.carrier_id.assign(mp::MaxCarrierIdBytes, 'c');
    BOOST_REQUIRE(wire::EncodeJoinRequest(join, bytes));
    auto oversized_join = join;
    oversized_join.authenticated_identity.push_back('i');
    BOOST_TEST(!wire::EncodeJoinRequest(oversized_join, bytes));
    oversized_join = join;
    oversized_join.request_id.push_back('r');
    BOOST_TEST(!wire::EncodeJoinRequest(oversized_join, bytes));
    oversized_join = join;
    oversized_join.carrier_id.push_back('c');
    BOOST_TEST(!wire::EncodeJoinRequest(oversized_join, bytes));

    auto query = QueryRequestValue();
    query.authenticated_identity.assign(mp::MaxIdentityBytes, 'i');
    query.request_id.assign(mp::MaxRequestIdBytes, 'r');
    query.carrier_id.assign(mp::MaxCarrierIdBytes, 'c');
    BOOST_REQUIRE(wire::EncodeQueryRequest(query, bytes));
    wire::QueryRequest decoded_query;
    BOOST_REQUIRE(wire::DecodeQueryRequest(bytes.data(), bytes.size(), decoded_query));
    BOOST_CHECK(Same(query, decoded_query));
    auto oversized_query = query;
    oversized_query.authenticated_identity.push_back('i');
    BOOST_TEST(!wire::EncodeQueryRequest(oversized_query, bytes));
    oversized_query = query;
    oversized_query.request_id.push_back('r');
    BOOST_TEST(!wire::EncodeQueryRequest(oversized_query, bytes));
    oversized_query = query;
    oversized_query.carrier_id.push_back('c');
    BOOST_TEST(!wire::EncodeQueryRequest(oversized_query, bytes));

    std::vector<std::uint8_t> base_context;
    BOOST_REQUIRE(wire::BuildCandidateBindingContext(query.client_session_id, query.request_id,
        query.carrier_id, query.client_nonce, base_context));
    auto changed_id = query.client_session_id;
    changed_id[0] ^= 1;
    std::vector<std::uint8_t> context;
    BOOST_REQUIRE(wire::BuildCandidateBindingContext(changed_id, query.request_id,
        query.carrier_id, query.client_nonce, context));
    BOOST_TEST(context != base_context);
    BOOST_REQUIRE(wire::BuildCandidateBindingContext(query.client_session_id, "other-request",
        query.carrier_id, query.client_nonce, context));
    BOOST_TEST(context != base_context);
    BOOST_REQUIRE(wire::BuildCandidateBindingContext(query.client_session_id, query.request_id,
        "other-carrier", query.client_nonce, context));
    BOOST_TEST(context != base_context);
    auto nonce = query.client_nonce;
    nonce[0] ^= 1;
    BOOST_REQUIRE(wire::BuildCandidateBindingContext(query.client_session_id, query.request_id,
        query.carrier_id, nonce, context));
    BOOST_TEST(context != base_context);
}

BOOST_AUTO_TEST_CASE(join_response_and_query_encoder_reject_invalid_values) {
    std::vector<std::uint8_t> bytes;
    mp::JoinResponse invalid_join;
    invalid_join.result = static_cast<mp::JoinResult>(255);
    BOOST_TEST(!wire::EncodeJoinResponse(invalid_join, bytes));

    wire::QueryResponse invalid_create;
    invalid_create.operation = wire::QueryOperation::Create;
    invalid_create.create = CreateResponseValue();
    invalid_create.create.grace_ms = wire::MaxGraceMilliseconds + 1;
    BOOST_TEST(!wire::EncodeQueryResponse(invalid_create, bytes));

    wire::QueryResponse invalid_operation;
    invalid_operation.operation = static_cast<wire::QueryOperation>(255);
    BOOST_TEST(!wire::EncodeQueryResponse(invalid_operation, bytes));
}

BOOST_AUTO_TEST_CASE(control_ping_and_pong_payloads_round_trip_and_reject_bad_shape) {
    for (const auto opcode : {wire::ControlOpcode::Ping, wire::ControlOpcode::Pong}) {
        wire::ControlPayload payload;
        payload.opcode = opcode;
        payload.nonce = Filled<8>(static_cast<std::uint8_t>(opcode));
        std::vector<std::uint8_t> bytes;
        BOOST_REQUIRE(wire::EncodeControlPayload(payload, bytes));
        BOOST_REQUIRE(bytes.size() == 9u);
        wire::ControlPayload decoded;
        BOOST_REQUIRE(wire::DecodeControlPayload(bytes.data(), bytes.size(), decoded));
        BOOST_CHECK(decoded.opcode == payload.opcode);
        BOOST_CHECK(decoded.nonce == payload.nonce);
        for (std::size_t size = 0; size < bytes.size(); ++size)
            BOOST_TEST(!wire::DecodeControlPayload(bytes.data(), size, decoded));
        bytes.push_back(0);
        BOOST_TEST(!wire::DecodeControlPayload(bytes.data(), bytes.size(), decoded));
        bytes.resize(9);
        bytes[0] = 0xff;
        BOOST_TEST(!wire::DecodeControlPayload(bytes.data(), bytes.size(), decoded));
        payload.opcode = static_cast<wire::ControlOpcode>(255);
        BOOST_TEST(!wire::EncodeControlPayload(payload, bytes));
    }
}

BOOST_AUTO_TEST_CASE(legacy_session_identifier_round_trips_as_fixed_big_endian_guid) {
#if defined(_PPP_INT128)
    const ppp::Int128 legacy(0x0123456789abcdefll, 0xfedcba9876543210ull);
#else
    const ppp::Int128 legacy = static_cast<ppp::Int128>(
        (static_cast<__uint128_t>(0x0123456789abcdefull) << 64) |
        static_cast<__uint128_t>(0xfedcba9876543210ull));
#endif
    const auto encoded = wire::ClientSessionIdFromInt128(legacy);
    BOOST_TEST(encoded[0] == 0x01u);
    BOOST_TEST(encoded[7] == 0xefu);
    BOOST_TEST(encoded[8] == 0xfeu);
    BOOST_TEST(encoded[15] == 0x10u);
    BOOST_CHECK(wire::ClientSessionIdToInt128(encoded) == legacy);

    auto wrong_legacy_id = encoded;
    wrong_legacy_id[15] ^= 1;
    BOOST_CHECK(wire::ClientSessionIdToInt128(wrong_legacy_id) != legacy);
}
