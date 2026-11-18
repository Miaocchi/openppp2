#define BOOST_TEST_MODULE multipath_secure_channel_test
#include <boost/test/included/unit_test.hpp>

#include <ppp/cryptography/noise/MultipathSecureChannel.h>

#include <openssl/crypto.h>

#include <array>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace noise = ppp::cryptography::noise;
namespace ppp::cryptography::noise {
struct MultipathSecureChannelTestAccess final {
    static void SetSequences(MultipathSecureChannel& channel,
        std::uint64_t send, std::uint64_t receive) noexcept {
        channel.send_sequence_ = send;
        channel.receive_sequence_ = receive;
    }
};
}

namespace {
noise::Secret32 Secret(std::uint8_t start) {
    noise::Bytes32 b{};
    for (std::size_t i = 0; i < b.size(); ++i) b[i] = static_cast<std::uint8_t>(start + i);
    return noise::Secret32(std::move(b));
}
std::string Hex(const std::vector<std::uint8_t>& b) {
    std::ostringstream s; s << std::hex << std::setfill('0');
    for (auto c : b) s << std::setw(2) << static_cast<unsigned>(c);
    return s.str();
}
noise::MultipathHandshakeParameters Parameters() {
    noise::MultipathHandshakeParameters p;
    p.client_identity = "client-a"; p.server_identity = "server-a"; p.key_id = "key-1";
    p.capabilities = {1, 2, 3};
    return p;
}
bool Pair(noise::MultipathSecureChannel& c, noise::MultipathSecureChannel& s,
          std::uint8_t psk_start = 0, std::vector<std::uint8_t>* saved_m1 = nullptr,
          std::vector<std::uint8_t>* saved_m2 = nullptr) {
    std::vector<std::uint8_t> m1, m2, none;
    if (!c.SetDeterministicEphemeralPrivateKeyForTesting(Secret(0x20)) ||
        !s.SetDeterministicEphemeralPrivateKeyForTesting(Secret(0x40)) ||
        !c.Start(noise::HandshakeRole::NetworkClientInitiator, Secret(psk_start), Parameters(), m1) ||
        !s.Start(noise::HandshakeRole::NetworkServerResponder, Secret(psk_start), Parameters(), none) ||
        !none.empty() || !s.ProcessHandshakeMessage(m1.data(), m1.size(), m2) ||
        !c.ProcessHandshakeMessage(m2.data(), m2.size(), none)) return false;
    if (saved_m1) *saved_m1 = m1;
    if (saved_m2) *saved_m2 = m2;
    return c.IsReady() && s.IsReady();
}

BOOST_AUTO_TEST_CASE(deterministic_noise_messages_match_the_v1_vector) {
    noise::MultipathSecureChannel c, s;
    std::vector<std::uint8_t> m1, m2;
    BOOST_REQUIRE(Pair(c, s, 0, &m1, &m2));
    BOOST_TEST(Hex(m1) ==
        "358072d6365880d1aeea329adf9121383851ed21a28e3b75e965d0d2cd166254"
        "c6acc417ab3bebce4905ccd359cade25");
    BOOST_TEST(Hex(m2) ==
        "79a631eede1bf9c98f12032cdeadd0e7a079398fc786b88cc846ec89af85a51a"
        "36ad93c1f60867b05d6d753b6b6fae09");
}
}

BOOST_AUTO_TEST_CASE(versioned_prologue_is_canonical_and_length_delimited) {
    std::vector<std::uint8_t> prologue;
    BOOST_REQUIRE(noise::MultipathSecureChannel::BuildPrologue(Parameters(), prologue));
    BOOST_TEST(Hex(prologue) ==
        "6f70656e707070322d6d756c7469706174682d7365637572652d6368616e6e656c2d7631"
        "00010008636c69656e742d6100087365727665722d6100056b65792d310003010203");
}

BOOST_AUTO_TEST_CASE(directional_records_are_deterministic_and_authenticated) {
    noise::MultipathSecureChannel c, s;
    BOOST_REQUIRE(Pair(c, s));
    const std::array<std::uint8_t, 4> payload{{9, 8, 7, 6}};
    std::vector<std::uint8_t> record, decoded;
    std::uint8_t type = 0;
    BOOST_REQUIRE(c.Seal(7, payload.data(), payload.size(), record));
    BOOST_TEST(Hex(record) == "000000157b090f7b65e3c4d103c817d62fcc26ceba132a794a");
    BOOST_REQUIRE(s.Open(record.data(), record.size(), type, decoded));
    BOOST_TEST(type == 7);
    BOOST_TEST(decoded == std::vector<std::uint8_t>(payload.begin(), payload.end()));
    std::vector<std::uint8_t> reverse_record;
    BOOST_REQUIRE(s.Seal(7, payload.data(), payload.size(), reverse_record));
    BOOST_TEST(Hex(reverse_record) == "0000001569c06d4da36bdf7cde45c476a81851455321d4b095");
    BOOST_REQUIRE(c.Open(reverse_record.data(), reverse_record.size(), type, decoded));
    BOOST_TEST(type == 7);
    BOOST_TEST(decoded == std::vector<std::uint8_t>(payload.begin(), payload.end()));
    const std::uint8_t label[] = {'j', 'o', 'i', 'n'};
    const std::uint8_t context_a[] = {1, 2};
    const std::uint8_t context_b[] = {1, 3};
    std::array<std::uint8_t, 64> exported_a{}, exported_b{};
    BOOST_REQUIRE(c.Export(label, sizeof(label), context_a, sizeof(context_a),
        exported_a.data(), exported_a.size()));
    BOOST_REQUIRE(c.Export(label, sizeof(label), context_b, sizeof(context_b),
        exported_b.data(), exported_b.size()));
    BOOST_TEST(Hex(std::vector<std::uint8_t>(exported_a.begin(), exported_a.end())) ==
        "ea243ede9e43a274d142a683b9153baf592002b31ae49b2f1eac623a9b4b381f"
        "daa6a8a4b51f10daafd3792e477915f94ed5e37990c98d705badb327c5f5f406");
    BOOST_TEST(exported_a != exported_b);
    BOOST_TEST(!s.Open(record.data(), record.size(), type, decoded));
    BOOST_TEST(s.IsFailed());
}

BOOST_AUTO_TEST_CASE(tamper_wrong_psk_and_transcript_mismatch_fail_closed) {
    noise::MultipathSecureChannel c, s;
    std::vector<std::uint8_t> m1, m2, none;
    BOOST_REQUIRE(c.SetDeterministicEphemeralPrivateKeyForTesting(Secret(0x20)));
    BOOST_REQUIRE(s.SetDeterministicEphemeralPrivateKeyForTesting(Secret(0x40)));
    BOOST_REQUIRE(c.Start(noise::HandshakeRole::NetworkClientInitiator, Secret(0), Parameters(), m1));
    BOOST_REQUIRE(s.Start(noise::HandshakeRole::NetworkServerResponder, Secret(1), Parameters(), none));
    BOOST_TEST(!s.ProcessHandshakeMessage(m1.data(), m1.size(), m2));
    BOOST_TEST(s.IsFailed());

    noise::MultipathSecureChannel c2, s2;
    BOOST_REQUIRE(Pair(c2, s2));
    const std::uint8_t byte = 1;
    std::vector<std::uint8_t> record;
    BOOST_REQUIRE(c2.Seal(1, &byte, 1, record));
    record.back() ^= 0x80;
    std::uint8_t type = 0; std::vector<std::uint8_t> plain;
    BOOST_TEST(!s2.Open(record.data(), record.size(), type, plain));
    BOOST_TEST(s2.IsFailed());
}

BOOST_AUTO_TEST_CASE(record_length_bounds_and_exact_frame_are_enforced) {
    noise::MultipathSecureChannel c, s;
    BOOST_REQUIRE(Pair(c, s));
    std::vector<std::uint8_t> oversized(noise::MultipathRecordMaxPlaintext + 1, 0), record;
    BOOST_TEST(!c.Seal(0, oversized.data(), oversized.size(), record));
    BOOST_TEST(c.IsFailed());

    noise::MultipathSecureChannel c2, s2;
    BOOST_REQUIRE(Pair(c2, s2));
    const std::uint8_t value = 42;
    BOOST_REQUIRE(c2.Seal(1, &value, 1, record));
    record.push_back(0);
    std::uint8_t type = 0; std::vector<std::uint8_t> plain;
    BOOST_TEST(!s2.Open(record.data(), record.size(), type, plain));
}

BOOST_AUTO_TEST_CASE(identity_and_capability_transcript_mismatch_fails) {
    noise::MultipathSecureChannel c, s;
    noise::MultipathHandshakeParameters client_params = Parameters();
    noise::MultipathHandshakeParameters server_params = Parameters();
    server_params.capabilities = {1, 2, 4};
    std::vector<std::uint8_t> m1, m2, none;
    BOOST_REQUIRE(c.SetDeterministicEphemeralPrivateKeyForTesting(Secret(0x20)));
    BOOST_REQUIRE(s.SetDeterministicEphemeralPrivateKeyForTesting(Secret(0x40)));
    BOOST_REQUIRE(c.Start(noise::HandshakeRole::NetworkClientInitiator, Secret(0), client_params, m1));
    BOOST_REQUIRE(s.Start(noise::HandshakeRole::NetworkServerResponder, Secret(0), server_params, none));
    BOOST_TEST(!s.ProcessHandshakeMessage(m1.data(), m1.size(), m2));
    BOOST_TEST(s.IsFailed());

    const auto identity_mismatch_fails = [](int field) {
        noise::MultipathSecureChannel client, server;
        auto client_params = Parameters();
        auto server_params = Parameters();
        if (field == 0) client_params.client_identity = "different-client";
        if (field == 1) client_params.server_identity = "different-server";
        if (field == 2) client_params.key_id = "different-key";
        std::vector<std::uint8_t> client_message, server_message, unused;
        return client.SetDeterministicEphemeralPrivateKeyForTesting(Secret(0x20)) &&
            server.SetDeterministicEphemeralPrivateKeyForTesting(Secret(0x40)) &&
            client.Start(noise::HandshakeRole::NetworkClientInitiator, Secret(0), client_params, client_message) &&
            server.Start(noise::HandshakeRole::NetworkServerResponder, Secret(0), server_params, unused) &&
            !server.ProcessHandshakeMessage(client_message.data(), client_message.size(), server_message) &&
            server.IsFailed();
    };
    BOOST_TEST(identity_mismatch_fails(0));
    BOOST_TEST(identity_mismatch_fails(1));
    BOOST_TEST(identity_mismatch_fails(2));
}

BOOST_AUTO_TEST_CASE(prologue_strings_require_strict_utf8) {
    auto parameters = Parameters();
    parameters.client_identity = "caf\xc3\xa9";
    parameters.server_identity = "server-\xf0\x9f\x94\x90";
    parameters.key_id = "key-\xe2\x82\xac";
    std::vector<std::uint8_t> prologue;
    BOOST_TEST(noise::MultipathSecureChannel::BuildPrologue(parameters, prologue));

    const std::vector<std::string> invalid = {
        std::string("\x80", 1),
        std::string("\xc0\xaf", 2),
        std::string("\xe0\x80\x80", 3),
        std::string("\xed\xa0\x80", 3),
        std::string("\xf4\x90\x80\x80", 4),
        std::string("\xe2\x82", 2),
        std::string("\xe2\x28\xa1", 3),
    };
    for (const auto& value : invalid) {
        parameters = Parameters();
        parameters.client_identity = value;
        BOOST_TEST(!noise::MultipathSecureChannel::BuildPrologue(parameters, prologue));
        parameters = Parameters();
        parameters.server_identity = value;
        BOOST_TEST(!noise::MultipathSecureChannel::BuildPrologue(parameters, prologue));
        parameters = Parameters();
        parameters.key_id = value;
        BOOST_TEST(!noise::MultipathSecureChannel::BuildPrologue(parameters, prologue));
    }
}

BOOST_AUTO_TEST_CASE(record_sequences_fail_closed_at_counter_exhaustion) {
    noise::MultipathSecureChannel client, server;
    BOOST_REQUIRE(Pair(client, server));
    noise::MultipathSecureChannelTestAccess::SetSequences(
        client, std::numeric_limits<std::uint64_t>::max(), 0);
    std::vector<std::uint8_t> record;
    const std::uint8_t byte = 1;
    BOOST_TEST(!client.Seal(1, &byte, 1, record));
    BOOST_TEST(client.IsFailed());

    noise::MultipathSecureChannel client2, server2;
    BOOST_REQUIRE(Pair(client2, server2));
    BOOST_REQUIRE(client2.Seal(1, &byte, 1, record));
    noise::MultipathSecureChannelTestAccess::SetSequences(
        server2, 0, std::numeric_limits<std::uint64_t>::max());
    std::uint8_t type = 0;
    std::vector<std::uint8_t> plaintext;
    BOOST_TEST(!server2.Open(record.data(), record.size(), type, plaintext));
    BOOST_TEST(server2.IsFailed());
}

BOOST_AUTO_TEST_CASE(fresh_random_handshakes_derive_distinct_exporters) {
    noise::MultipathSecureChannel client_a, server_a, client_b, server_b;
    const auto complete_random_pair = [](noise::MultipathSecureChannel& client,
        noise::MultipathSecureChannel& server) {
        std::vector<std::uint8_t> first, second, unused;
        return client.Start(noise::HandshakeRole::NetworkClientInitiator, Secret(0), Parameters(), first) &&
            server.Start(noise::HandshakeRole::NetworkServerResponder, Secret(0), Parameters(), unused) &&
            unused.empty() && server.ProcessHandshakeMessage(first.data(), first.size(), second) &&
            client.ProcessHandshakeMessage(second.data(), second.size(), unused) && unused.empty();
    };
    BOOST_REQUIRE(complete_random_pair(client_a, server_a));
    BOOST_REQUIRE(complete_random_pair(client_b, server_b));
    const std::uint8_t label[] = {'r', 'e', 'c', 'o', 'n', 'n', 'e', 'c', 't'};
    std::array<std::uint8_t, 32> exporter_a{}, exporter_b{};
    BOOST_REQUIRE(client_a.Export(label, sizeof(label), nullptr, 0,
        exporter_a.data(), exporter_a.size()));
    BOOST_REQUIRE(client_b.Export(label, sizeof(label), nullptr, 0,
        exporter_b.data(), exporter_b.size()));
    BOOST_TEST(exporter_a != exporter_b);
    OPENSSL_cleanse(exporter_a.data(), exporter_a.size());
    OPENSSL_cleanse(exporter_b.data(), exporter_b.size());
}
