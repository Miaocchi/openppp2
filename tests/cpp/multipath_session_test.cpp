#define BOOST_TEST_MODULE multipath_session_test
#include <boost/test/included/unit_test.hpp>

#include <ppp/app/multipath/Control.h>
#include <ppp/app/multipath/Join.h>
#include <ppp/app/multipath/Session.h>

#include <openssl/rand.h>

#include <array>
#include <chrono>
#include <cstdint>

namespace mp = ppp::app::multipath;
using namespace std::chrono_literals;

namespace {
mp::TimePoint T(std::int64_t ms) { return mp::TimePoint{} + std::chrono::milliseconds(ms); }

mp::JoinRequest MakeRequest(const mp::SessionTicket& ticket, mp::JoinOperation op,
                            const std::string& request_id, const std::string& carrier,
                            std::uint8_t seed) {
    mp::JoinRequest request;
    request.operation = op;
    request.authenticated_identity = "principal-7";
    request.request_id = request_id;
    request.carrier_id = carrier;
    request.session_id = ticket.id;
    request.client_session_id = ticket.client_session_id;
    request.generation = ticket.generation;
    for (std::size_t i = 0; i < request.client_nonce.size(); ++i) {
        request.client_nonce[i] = static_cast<std::uint8_t>(seed + i);
        request.candidate_binding[i] = static_cast<std::uint8_t>(seed + i + 64);
    }
    BOOST_REQUIRE(mp::ComputeJoinProof(ticket.credential.Value(), request, request.proof));
    return request;
}

mp::SessionTicket Create(mp::SessionState& session, mp::TimePoint now) {
    mp::SessionTicket ticket;
    mp::SessionId legacy_id{};
    legacy_id[15] = 7;
    BOOST_REQUIRE(session.Create("principal-7", legacy_id, now, 45s, ticket));
    BOOST_CHECK(ticket.grace == std::chrono::duration_cast<mp::Duration>(30s));
    return ticket;
}
}

BOOST_AUTO_TEST_CASE(create_generates_ticket_and_clamps_grace_without_exposing_secret) {
    mp::SessionState session;
    auto ticket = Create(session, T(10));
    auto snapshot = session.Snapshot();
    BOOST_CHECK(snapshot.status == mp::SessionStatus::Active);
    BOOST_TEST(snapshot.generation == 1u);
    BOOST_TEST(snapshot.carrier_count == 0u);
    BOOST_CHECK(snapshot.id == ticket.id);
    BOOST_TEST(ticket.credential.IsSet());
    BOOST_TEST(!session.Create("", ticket.client_session_id, T(10), 1s, ticket));
}

BOOST_AUTO_TEST_CASE(join_reserve_confirm_commit_and_lost_final_ack_are_idempotent) {
    mp::SessionState session;
    auto ticket = Create(session, T(0));
    mp::JoinCoordinator joins;
    auto reserve = MakeRequest(ticket, mp::JoinOperation::Join, "req-1", "carrier-a", 1);
    reserve.peer_committed_operation_sequence = 2;
    BOOST_REQUIRE(mp::ComputeJoinProof(ticket.credential.Value(), reserve, reserve.proof));
    mp::JoinResponse reserved;
    BOOST_CHECK(joins.Reserve(session, reserve, T(1), reserved, 4, 1) == mp::JoinResult::Rejected);
    BOOST_CHECK(joins.Reserve(session, reserve, T(1), reserved, 4, 2) == mp::JoinResult::Accepted);
    BOOST_TEST(reserved.server_committed_operation_sequence == 4u);
    auto altered_peer_watermark = reserve;
    ++altered_peer_watermark.peer_committed_operation_sequence;
    BOOST_TEST(!mp::VerifyJoinProof(ticket.credential.Value(), altered_peer_watermark));
    BOOST_CHECK(reserved.server_nonce != mp::Secret32{});

    mp::JoinResponse duplicate;
    BOOST_CHECK(joins.Reserve(session, reserve, T(2), duplicate, 99, 2) == mp::JoinResult::Duplicate);
    BOOST_CHECK(duplicate.server_nonce == reserved.server_nonce);
    BOOST_TEST(duplicate.server_committed_operation_sequence == 4u);

    auto confirm = reserve;
    confirm.phase = mp::JoinPhase::Confirm;
    confirm.server_nonce = reserved.server_nonce;
    BOOST_REQUIRE(mp::ComputeJoinProof(ticket.credential.Value(), confirm, confirm.proof));
    mp::JoinResponse response;
    BOOST_CHECK(joins.Confirm(session, confirm, T(3), response) == mp::JoinResult::Accepted);
    const auto confirmed = response;
    BOOST_CHECK(joins.Confirm(session, confirm, T(3), response) == mp::JoinResult::Duplicate);
    BOOST_CHECK(response.server_nonce == confirmed.server_nonce);
    BOOST_CHECK(response.proof == confirmed.proof);
    BOOST_TEST(response.server_committed_operation_sequence == 4u);

    auto commit = confirm;
    commit.phase = mp::JoinPhase::Commit;
    BOOST_REQUIRE(mp::ComputeJoinProof(ticket.credential.Value(), commit, commit.proof));
    const auto commit_retry = commit;
    BOOST_CHECK(joins.Commit(session, commit, 12, T(4), response) == mp::JoinResult::Committed);
    commit.proof = response.proof;
    BOOST_CHECK(mp::VerifyJoinResponseProof(ticket.credential.Value(), commit,
        response.server_committed_operation_sequence, response.proof));
    BOOST_TEST(response.server_committed_operation_sequence == 4u);
    BOOST_TEST(session.Snapshot().carrier_count == 1u);
    BOOST_CHECK(joins.Query("other-principal", "req-1", T(5), response) == mp::JoinResult::Rejected);
    BOOST_CHECK(joins.Query("principal-7", "req-1", T(5), response) == mp::JoinResult::Committed);
    BOOST_TEST(response.generation == 1u);
    BOOST_TEST(response.server_committed_operation_sequence == 4u);
    auto altered_response_watermark = response.server_committed_operation_sequence + 1;
    BOOST_TEST(!mp::VerifyJoinResponseProof(ticket.credential.Value(), commit,
        altered_response_watermark, response.proof));
    auto altered_request = commit;
    ++altered_request.peer_committed_operation_sequence;
    BOOST_TEST(!mp::VerifyJoinResponseProof(ticket.credential.Value(), altered_request,
        response.server_committed_operation_sequence, response.proof));
    BOOST_CHECK(joins.Commit(session, commit_retry, 12, T(6), response) == mp::JoinResult::Committed);
    BOOST_TEST(session.Snapshot().carrier_count == 1u);
    BOOST_CHECK(joins.Query("principal-7", "req-1", T(10002), response) == mp::JoinResult::NotFound);
}

BOOST_AUTO_TEST_CASE(join_rejects_wrong_identity_exporter_binding_nonce_and_duplicate_carrier) {
    mp::SessionState session;
    auto ticket = Create(session, T(0));
    mp::JoinCoordinator joins;
    auto request = MakeRequest(ticket, mp::JoinOperation::Join, "req-a", "same", 2);
    request.authenticated_identity = "other-principal";
    BOOST_REQUIRE(mp::ComputeJoinProof(ticket.credential.Value(), request, request.proof));
    mp::JoinResponse response;
    BOOST_CHECK(joins.Reserve(session, request, T(1), response) == mp::JoinResult::Rejected);

    request = MakeRequest(ticket, mp::JoinOperation::Join, "req-b", "same", 3);
    request.candidate_binding[0] ^= 1;
    BOOST_CHECK(joins.Reserve(session, request, T(1), response) == mp::JoinResult::Rejected);

    request = MakeRequest(ticket, mp::JoinOperation::Join, "req-b2", "same", 31);
    request.session_id[0] ^= 1;
    BOOST_REQUIRE(mp::ComputeJoinProof(ticket.credential.Value(), request, request.proof));
    BOOST_CHECK(joins.Reserve(session, request, T(1), response) == mp::JoinResult::Rejected);

    request = MakeRequest(ticket, mp::JoinOperation::Join, "req-b3", "same", 32);
    request.client_session_id[0] ^= 1;
    BOOST_REQUIRE(mp::ComputeJoinProof(ticket.credential.Value(), request, request.proof));
    BOOST_CHECK(joins.Reserve(session, request, T(1), response) == mp::JoinResult::Rejected);

    request = MakeRequest(ticket, mp::JoinOperation::Join, "req-b4", "same", 33);
    ++request.generation;
    BOOST_REQUIRE(mp::ComputeJoinProof(ticket.credential.Value(), request, request.proof));
    BOOST_CHECK(joins.Reserve(session, request, T(1), response) == mp::JoinResult::Rejected);

    request = MakeRequest(ticket, mp::JoinOperation::Join, "req-c", "same", 4);
    request.client_nonce.fill(0);
    BOOST_CHECK(joins.Reserve(session, request, T(1), response) == mp::JoinResult::Rejected);

    request = MakeRequest(ticket, mp::JoinOperation::Join, "req-d", "same", 5);
    BOOST_REQUIRE(joins.Reserve(session, request, T(1), response) == mp::JoinResult::Accepted);
    auto confirm = request;
    confirm.phase = mp::JoinPhase::Confirm;
    confirm.server_nonce = response.server_nonce;
    confirm.candidate_binding[1] ^= 1;
    BOOST_REQUIRE(mp::ComputeJoinProof(ticket.credential.Value(), confirm, confirm.proof));
    BOOST_CHECK(joins.Confirm(session, confirm, T(2), response) == mp::JoinResult::Rejected);
}

BOOST_AUTO_TEST_CASE(duplicate_carrier_identity_cannot_replace_committed_carrier) {
    mp::SessionState session;
    auto ticket = Create(session, T(0));
    BOOST_REQUIRE(session.OnCarrierJoined("carrier-x", 1, 44, T(1)));
    BOOST_TEST(!session.OnCarrierJoined("carrier-x", 1, 45, T(2)));
    BOOST_TEST(session.Snapshot().carrier_count == 1u);
    BOOST_TEST(!session.OnCarrierJoined("other", 2, 46, T(3)));
    BOOST_TEST(session.Snapshot().carrier_count == 1u);
    BOOST_TEST(ticket.generation == 1u);
}

BOOST_AUTO_TEST_CASE(carrier_table_has_a_hard_bound_and_stale_generation_cannot_detach) {
    mp::SessionState session;
    auto ticket = Create(session, T(0));
    for (std::size_t i = 0; i < mp::MaxCarriers; ++i)
        BOOST_REQUIRE(session.OnCarrierJoined("carrier-" + std::to_string(i), 1, i + 1, T(1)));
    BOOST_TEST(session.Snapshot().carrier_count == mp::MaxCarriers);
    BOOST_TEST(!session.OnCarrierJoined("carrier-overflow", 1, 99, T(2)));
    BOOST_TEST(!session.OnCarrierLeft("carrier-0", 2, 1, T(3)));
    BOOST_TEST(session.Snapshot().carrier_count == mp::MaxCarriers);
    BOOST_TEST(ticket.generation == 1u);
}

BOOST_AUTO_TEST_CASE(join_pending_table_is_bounded_and_cancel_releases_the_slot) {
    mp::SessionState session;
    auto ticket = Create(session, T(0));
    mp::JoinCoordinator joins;
    mp::JoinResponse response;
    for (std::size_t i = 0; i < mp::MaxJoinTransactions; ++i) {
        auto request = MakeRequest(ticket, mp::JoinOperation::Join,
            "pending-" + std::to_string(i), "carrier-" + std::to_string(i),
            static_cast<std::uint8_t>(i + 1));
        BOOST_REQUIRE(joins.Reserve(session, request, T(1), response) == mp::JoinResult::Accepted);
    }
    BOOST_TEST(joins.PendingCount() == mp::MaxJoinTransactions);
    auto full = MakeRequest(ticket, mp::JoinOperation::Join, "over-capacity", "overflow", 99);
    BOOST_CHECK(joins.Reserve(session, full, T(2), response) == mp::JoinResult::Capacity);
    joins.Cancel("pending-0");
    BOOST_CHECK(joins.Query("principal-7", "pending-0", T(3), response) == mp::JoinResult::NotFound);
    auto replacement = MakeRequest(ticket, mp::JoinOperation::Join, "replacement", "replacement", 100);
    BOOST_CHECK(joins.Reserve(session, replacement, T(4), response) == mp::JoinResult::Accepted);
    BOOST_TEST(joins.PendingCount() == mp::MaxJoinTransactions);
}

BOOST_AUTO_TEST_CASE(last_carrier_departure_sets_one_fixed_deadline_failed_candidates_do_not_extend_it) {
    mp::SessionState session;
    auto ticket = Create(session, T(0));
    BOOST_REQUIRE(session.OnCarrierJoined("a", 1, 1, T(1)));
    BOOST_REQUIRE(session.OnCarrierJoined("b", 1, 2, T(2)));
    BOOST_REQUIRE(session.OnCarrierLeft("a", 1, 1, T(3)));
    BOOST_CHECK(session.Snapshot().status == mp::SessionStatus::Active);
    BOOST_REQUIRE(session.OnCarrierLeft("b", 1, 2, T(4)));
    const auto deadline = session.Snapshot().deadline;
    BOOST_CHECK(deadline == T(4) + std::chrono::duration_cast<mp::Duration>(30s));

    mp::JoinCoordinator joins;
    auto invalid = MakeRequest(ticket, mp::JoinOperation::Resume, "bad", "candidate", 10);
    invalid.proof[0] ^= 1;
    mp::JoinResponse response;
    BOOST_CHECK(joins.Reserve(session, invalid, T(5000), response) == mp::JoinResult::Rejected);
    BOOST_CHECK(session.Snapshot().deadline == deadline);

    auto resume = MakeRequest(ticket, mp::JoinOperation::Resume, "resume", "first", 11);
    auto failed = MakeRequest(ticket, mp::JoinOperation::Resume, "failed", "failed-carrier", 12);
    BOOST_REQUIRE(joins.Reserve(session, failed, T(6000), response) == mp::JoinResult::Accepted);
    auto failed_confirm = failed;
    failed_confirm.phase = mp::JoinPhase::Confirm;
    failed_confirm.server_nonce = response.server_nonce;
    failed_confirm.client_session_id[0] ^= 1;
    BOOST_REQUIRE(mp::ComputeJoinProof(ticket.credential.Value(), failed_confirm, failed_confirm.proof));
    BOOST_CHECK(joins.Confirm(session, failed_confirm, T(6001), response) == mp::JoinResult::Rejected);
    BOOST_CHECK(session.Snapshot().deadline == deadline);
    BOOST_CHECK(session.Snapshot().status == mp::SessionStatus::Resuming);

    BOOST_REQUIRE(joins.Reserve(session, resume, T(16001), response) == mp::JoinResult::Accepted);
    auto confirm = resume;
    confirm.phase = mp::JoinPhase::Confirm;
    confirm.server_nonce = response.server_nonce;
    BOOST_REQUIRE(mp::ComputeJoinProof(ticket.credential.Value(), confirm, confirm.proof));
    BOOST_REQUIRE(joins.Confirm(session, confirm, T(16002), response) == mp::JoinResult::Accepted);
    auto commit = confirm;
    commit.phase = mp::JoinPhase::Commit;
    BOOST_REQUIRE(mp::ComputeJoinProof(ticket.credential.Value(), commit, commit.proof));
    BOOST_REQUIRE(joins.Commit(session, commit, 8, T(16003), response) == mp::JoinResult::Committed);
    BOOST_TEST(session.Snapshot().generation == 2u);
    BOOST_CHECK(session.Snapshot().status == mp::SessionStatus::Active);
    BOOST_TEST(!session.OnCarrierLeft("first", 1, 8, T(16004)));
    BOOST_TEST(session.Snapshot().carrier_count == 1u);
    BOOST_REQUIRE(session.OnCarrierLeft("first", 2, 8, T(16004)));
}

BOOST_AUTO_TEST_CASE(expiry_clears_credentials_and_rejects_old_async_callbacks) {
    mp::SessionState session;
    auto ticket = Create(session, T(0));
    BOOST_REQUIRE(session.OnCarrierJoined("first", 1, 8, T(1)));
    BOOST_REQUIRE(session.OnCarrierLeft("first", 1, 8, T(2)));
    BOOST_TEST(!session.Expire(T(30001)));
    BOOST_TEST(session.Expire(T(30002)));
    BOOST_CHECK(session.Snapshot().status == mp::SessionStatus::Expired);
    BOOST_TEST(!session.Credential().IsSet());
    BOOST_TEST(!session.OnCarrierJoined("late", 1, 9, T(30003)));
    BOOST_TEST(!session.OnCarrierLeft("first", 1, 8, T(30003)));
}

BOOST_AUTO_TEST_CASE(abandoned_resume_releases_attempt_without_moving_grace_deadline) {
    mp::SessionState session;
    auto ticket = Create(session, T(0));
    BOOST_REQUIRE(session.OnCarrierJoined("first", 1, 1, T(1)));
    BOOST_REQUIRE(session.OnCarrierLeft("first", 1, 1, T(2)));
    const auto deadline = session.Snapshot().deadline;
    BOOST_REQUIRE(session.BeginResume(ticket.id, ticket.generation, "principal-7", "attempt-1", T(3)));
    BOOST_CHECK(session.Snapshot().status == mp::SessionStatus::Resuming);
    BOOST_TEST(session.BeginResume(ticket.id, ticket.generation, "principal-7", "attempt-2", T(10004)));
    BOOST_CHECK(session.Snapshot().status == mp::SessionStatus::Resuming);
    BOOST_CHECK(session.Snapshot().deadline == deadline);
}

BOOST_AUTO_TEST_CASE(reliable_control_bounds_window_and_deduplicates_operations) {
    mp::ReliableControlState sender(7);
    mp::ReliableControlState receiver(7);
    const std::uint8_t payload[] = {1, 2, 3};
    mp::ControlMessage message;
    for (std::size_t i = 0; i < mp::MaxControlWindow; ++i)
        BOOST_REQUIRE(sender.MakeMessage(7, i + 1, "op-" + std::to_string(i), payload, sizeof(payload), message));
    BOOST_TEST(!sender.MakeMessage(7, 65, "blocked", payload, sizeof(payload), message));
    mp::ControlMessage retry;
    BOOST_REQUIRE(sender.MakeMessage(7, 64, "op-63", payload, sizeof(payload), retry));
    BOOST_TEST(retry.sequence == 64u);
    BOOST_REQUIRE(sender.Acknowledge(10));
    BOOST_TEST(sender.InFlight() == mp::MaxControlWindow - 10);
    BOOST_TEST(!sender.MakeMessage(7, 1, "op-0", payload, sizeof(payload), message));

    message.sequence = 2;
    message.session_generation = 7;
    message.operation_sequence = 2;
    message.operation_id = "dedupe";
    std::vector<mp::ControlDelivery> ready;
    BOOST_CHECK(receiver.Receive(message, ready) == mp::ControlReceive::Buffered);
    BOOST_TEST(ready.empty());
    BOOST_TEST(receiver.LastAcknowledged() == 0u);
    message.sequence = 1;
    message.operation_sequence = 1;
    BOOST_CHECK(receiver.Receive(message, ready) == mp::ControlReceive::Delivered);
    BOOST_REQUIRE(ready.size() == 2u);
    BOOST_TEST(ready[0].message.sequence == 1u);
    BOOST_TEST(ready[0].apply_operation);
    BOOST_TEST(ready[1].message.sequence == 2u);
    BOOST_TEST(ready[1].apply_operation);
    BOOST_TEST(receiver.LastAcknowledged() == 0u);
    BOOST_REQUIRE(receiver.CommitReceived(2));
    BOOST_TEST(receiver.LastAcknowledged() == 2u);
    message.sequence = 3;
    message.operation_sequence = 2;
    BOOST_CHECK(receiver.Receive(message, ready) == mp::ControlReceive::Delivered);
    BOOST_REQUIRE(ready.size() == 1u);
    BOOST_TEST(!ready[0].apply_operation);
    BOOST_REQUIRE(receiver.CommitReceived(3));
    BOOST_TEST(receiver.LastReceived() == 3u);

    mp::ReliableControlState gap_receiver(8);
    message = mp::ControlMessage{};
    message.session_generation = 8;
    message.sequence = 2;
    message.operation_sequence = 2;
    message.operation_id = "buffered-2";
    BOOST_CHECK(gap_receiver.Receive(message, ready) == mp::ControlReceive::Buffered);
    BOOST_TEST(gap_receiver.BufferedOutOfOrder() == 1u);
    std::vector<mp::ControlMessage> replay;
    BOOST_REQUIRE(gap_receiver.AdvanceGeneration(9, 0, replay));
    BOOST_TEST(gap_receiver.BufferedOutOfOrder() == 0u);
    for (std::uint64_t sequence = 1; sequence <= 2; ++sequence) {
        message = mp::ControlMessage{};
        message.session_generation = 9;
        message.sequence = sequence;
        message.operation_sequence = sequence;
        message.operation_id = "replayed-" + std::to_string(sequence);
        const auto result = gap_receiver.Receive(message, ready);
        BOOST_CHECK(result == mp::ControlReceive::Delivered);
        BOOST_REQUIRE(ready.size() == 1u);
        BOOST_TEST(ready[0].apply_operation);
        BOOST_REQUIRE(gap_receiver.CommitReceived(sequence));
    }
    BOOST_TEST(gap_receiver.ReceiveOperationCommitted() == 2u);

    mp::ReliableControlState bidirectional(9);
    for (std::uint64_t seq = 1; seq <= 5; ++seq) {
        message.session_generation = 9;
        message.sequence = seq;
        message.operation_sequence = seq;
        message.operation_id = "incoming-" + std::to_string(seq);
        BOOST_CHECK(bidirectional.Receive(message, ready) == mp::ControlReceive::Delivered);
        BOOST_REQUIRE(bidirectional.CommitReceived(seq));
    }
    BOOST_REQUIRE(bidirectional.MakeMessage(9, 1, "out-1", payload, sizeof(payload), message));
    BOOST_REQUIRE(message.acknowledged == 5u);
    BOOST_REQUIRE(bidirectional.MakeMessage(9, 2, "out-2", payload, sizeof(payload), message));
    BOOST_REQUIRE(bidirectional.Acknowledge(2));
    BOOST_TEST(bidirectional.PeerAcknowledged() == 2u);
    BOOST_TEST(bidirectional.LastAcknowledged() == 5u);
    std::vector<mp::ControlMessage> resend;
    BOOST_REQUIRE(bidirectional.GetUnacknowledged(resend));
    BOOST_REQUIRE(resend.size() == 0u);

    mp::ControlMessage ack_only;
    BOOST_REQUIRE(bidirectional.MakeAckOnly(ack_only));
    BOOST_TEST(ack_only.acknowledgement_only);
    BOOST_TEST(ack_only.session_generation == 9u);
    BOOST_TEST(ack_only.sequence == 0u);
    BOOST_TEST(ack_only.operation_sequence == 0u);
    BOOST_TEST(ack_only.acknowledged == 5u);
    mp::ReliableControlState ack_reader(9);
    for (std::uint64_t sequence = 1; sequence <= 5; ++sequence)
        BOOST_REQUIRE(ack_reader.MakeMessage(9, sequence, "sent-" + std::to_string(sequence),
            payload, sizeof(payload), message));
    BOOST_CHECK(ack_reader.Receive(ack_only, ready) == mp::ControlReceive::AckOnly);
    BOOST_TEST(ack_reader.PeerAcknowledged() == 5u);

    mp::ReliableControlState long_lived(12);
    for (std::uint64_t sequence = 1; sequence <= 10000; ++sequence) {
        message = mp::ControlMessage{};
        message.session_generation = 12;
        message.sequence = sequence;
        message.operation_sequence = sequence;
        message.operation_id = "routine";
        BOOST_REQUIRE(long_lived.Receive(message, ready) == mp::ControlReceive::Delivered);
        BOOST_REQUIRE(ready.size() == 1u && ready[0].apply_operation);
        BOOST_REQUIRE(long_lived.CommitReceived(sequence));
    }
    BOOST_TEST(long_lived.ReceiveOperationCommitted() == 10000u);
    message.sequence = 10001;
    message.operation_sequence = 1;
    message.operation_id = "old-retry";
    BOOST_CHECK(long_lived.Receive(message, ready) == mp::ControlReceive::Delivered);
    BOOST_REQUIRE(ready.size() == 1u);
    BOOST_TEST(!ready[0].apply_operation);
    BOOST_REQUIRE(long_lived.CommitReceived(10001));
    BOOST_TEST(long_lived.ReceiveOperationCommitted() == 10000u);

    long_lived.Reset(13);
    message.session_generation = 12;
    message.sequence = 10002;
    BOOST_CHECK(long_lived.Receive(message, ready) == mp::ControlReceive::Invalid);
}

BOOST_AUTO_TEST_CASE(control_resume_reconciles_both_directions_after_lost_acks) {
    const std::uint8_t body[] = {4, 5};
    mp::ReliableControlState left(20);
    mp::ReliableControlState right(20);
    mp::ControlMessage left_one, left_two, right_one, right_two;
    BOOST_REQUIRE(left.MakeMessage(20, 1, "left-1", body, sizeof(body), left_one));
    BOOST_REQUIRE(left.MakeMessage(20, 2, "left-2", body, sizeof(body), left_two));
    BOOST_REQUIRE(right.MakeMessage(20, 1, "right-1", body, sizeof(body), right_one));
    BOOST_REQUIRE(right.MakeMessage(20, 2, "right-2", body, sizeof(body), right_two));

    std::vector<mp::ControlDelivery> ready;
    BOOST_CHECK(left.Receive(right_one, ready) == mp::ControlReceive::Delivered);
    BOOST_REQUIRE(left.CommitReceived(1));
    BOOST_CHECK(right.Receive(left_one, ready) == mp::ControlReceive::Delivered);
    BOOST_REQUIRE(right.CommitReceived(1));
    mp::ControlMessage lost_left_ack, lost_right_ack;
    BOOST_REQUIRE(left.MakeAckOnly(lost_left_ack));
    BOOST_REQUIRE(right.MakeAckOnly(lost_right_ack));
    BOOST_TEST(lost_left_ack.acknowledged_operation_sequence == 1u);
    BOOST_TEST(lost_right_ack.acknowledged_operation_sequence == 1u);

    std::vector<mp::ControlMessage> left_replay;
    std::vector<mp::ControlMessage> right_replay;
    BOOST_REQUIRE(left.AdvanceGeneration(21, right.ReceiveOperationCommitted(), left_replay));
    BOOST_REQUIRE(right.AdvanceGeneration(21, left.ReceiveOperationCommitted(), right_replay));
    BOOST_REQUIRE(left_replay.size() == 1u);
    BOOST_REQUIRE(right_replay.size() == 1u);
    BOOST_TEST(left_replay[0].session_generation == 21u);
    BOOST_TEST(left_replay[0].sequence == 1u);
    BOOST_TEST(left_replay[0].operation_sequence == 2u);
    BOOST_TEST(right_replay[0].operation_sequence == 2u);
    BOOST_TEST(left.LastOperationAllocated() == 2u);
    BOOST_TEST(left.ReceiveOperationCommitted() == 1u);
    BOOST_TEST(left.PeerOperationAcknowledged() == 1u);

    BOOST_CHECK(right.Receive(left_replay[0], ready) == mp::ControlReceive::Delivered);
    BOOST_REQUIRE(ready.size() == 1u && ready[0].apply_operation);
    BOOST_REQUIRE(right.CommitReceived(1));
    BOOST_CHECK(left.Receive(right_replay[0], ready) == mp::ControlReceive::Delivered);
    BOOST_REQUIRE(ready.size() == 1u && ready[0].apply_operation);
    BOOST_REQUIRE(left.CommitReceived(1));
    BOOST_TEST(left.ReceiveOperationCommitted() == 2u);
    BOOST_TEST(right.ReceiveOperationCommitted() == 2u);

    mp::ControlMessage impossible;
    BOOST_TEST(!left.AdvanceGeneration(22, 99, left_replay));
    BOOST_TEST(left.SnapshotGeneration() == 21u);
    BOOST_TEST(!left.MakeMessage(21, 1, "left-1", body, sizeof(body), impossible));
    BOOST_REQUIRE(left.MakeMessage(21, 3, "left-3", body, sizeof(body), impossible));
    BOOST_TEST(impossible.sequence == 2u);
}

BOOST_AUTO_TEST_CASE(invalid_control_operation_gap_does_not_commit_ack_or_sequence) {
    const std::uint8_t body[] = {1};
    mp::ReliableControlState state(3);
    mp::ControlMessage outbound;
    BOOST_REQUIRE(state.MakeMessage(3, 1, "local", body, sizeof(body), outbound));
    mp::ControlMessage invalid;
    invalid.session_generation = 3;
    invalid.sequence = 1;
    invalid.acknowledged = 1;
    invalid.operation_sequence = 2;
    invalid.operation_id = "gap";
    std::vector<mp::ControlDelivery> ready;
    BOOST_CHECK(state.Receive(invalid, ready) == mp::ControlReceive::Invalid);
    BOOST_TEST(state.PeerAcknowledged() == 0u);
    BOOST_TEST(state.LastReceived() == 0u);
    BOOST_TEST(state.InFlight() == 1u);

    invalid.acknowledgement_only = true;
    invalid.sequence = 1;
    invalid.operation_sequence = 0;
    invalid.operation_id.clear();
    BOOST_CHECK(state.Receive(invalid, ready) == mp::ControlReceive::Invalid);
    BOOST_TEST(state.PeerAcknowledged() == 0u);
}

BOOST_AUTO_TEST_CASE(ack_only_commits_operation_watermark_without_ack_of_ack) {
    const std::uint8_t body[] = {7};
    mp::ReliableControlState sender(5);
    mp::ReliableControlState receiver(5);
    mp::ControlMessage message;
    BOOST_REQUIRE(sender.MakeMessage(5, 1, "one", body, sizeof(body), message));
    BOOST_REQUIRE(sender.MakeMessage(5, 2, "two", body, sizeof(body), message));
    std::vector<mp::ControlDelivery> ready;
    BOOST_CHECK(receiver.Receive(message, ready) == mp::ControlReceive::Buffered);
    message = mp::ControlMessage{};
    message.session_generation = 5;
    message.sequence = 1;
    message.operation_sequence = 1;
    message.operation_id = "one";
    BOOST_CHECK(receiver.Receive(message, ready) == mp::ControlReceive::Delivered);
    BOOST_REQUIRE(receiver.CommitReceived(2));

    mp::ControlMessage ack;
    BOOST_REQUIRE(receiver.MakeAckOnly(ack));
    BOOST_TEST(ack.sequence == 0u);
    BOOST_TEST(ack.acknowledged == 2u);
    BOOST_TEST(ack.acknowledged_operation_sequence == 2u);
    BOOST_CHECK(sender.Receive(ack, ready) == mp::ControlReceive::AckOnly);
    BOOST_TEST(sender.PeerAcknowledged() == 2u);
    BOOST_TEST(sender.PeerOperationAcknowledged() == 2u);
    BOOST_TEST(sender.InFlight() == 0u);
    BOOST_REQUIRE(sender.MakeMessage(5, 3, "three", body, sizeof(body), message));
    BOOST_TEST(message.sequence == 3u);
}
