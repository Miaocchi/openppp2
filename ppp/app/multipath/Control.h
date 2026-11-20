#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace ppp::app::multipath {

inline constexpr std::size_t MaxControlWindow = 64;
inline constexpr std::size_t MaxControlPayload = 4096;
inline constexpr std::size_t MaxControlOperationId = 64;
struct ControlMessage final {
    std::uint64_t session_generation = 1;
    std::uint64_t sequence = 0;
    std::uint64_t acknowledged = 0;
    std::uint64_t operation_sequence = 0;
    std::uint64_t acknowledged_operation_sequence = 0;
    bool acknowledgement_only = false;
    std::string operation_id;
    std::vector<std::uint8_t> payload;
};

struct ControlDelivery final {
    ControlMessage message;
    bool apply_operation = false;
};

enum class ControlReceive : std::uint8_t {
    Delivered, Buffered, Duplicate, AckOnly, OutOfWindow, Capacity, Invalid
};

/**
 * Bounded reliable control state, serialized by the session owner. Control
 * sequence numbers restart per generation; operation numbers increase across
 * generations for a logical session. A retry reuses its operation number and
 * returns the original in-flight frame. AdvanceGeneration retains operation
 * watermarks and reframes uncommitted work. Reset is for a new logical session.
 * ACK-only messages carry committed control and operation watermarks without
 * creating a sequence number or operation.
 */
class ReliableControlState final {
public:
    explicit ReliableControlState(std::uint64_t session_generation = 1) noexcept
        : session_generation_(session_generation) {}
    bool MakeMessage(std::uint64_t session_generation, std::uint64_t operation_sequence,
                     const std::string& operation_id, const std::uint8_t* payload,
                     std::size_t size, ControlMessage& output) noexcept;
    bool MakeAckOnly(ControlMessage& output) const noexcept;
    bool AdvanceGeneration(std::uint64_t new_generation,
                           std::uint64_t peer_committed_operation_sequence,
                           std::vector<ControlMessage>& replay) noexcept;
    bool GetUnacknowledged(std::vector<ControlMessage>& output) const noexcept;
    bool Acknowledge(std::uint64_t contiguous_sequence) noexcept;
    ControlReceive Receive(const ControlMessage& message,
                          std::vector<ControlDelivery>& ready) noexcept;
    bool CommitReceived(std::uint64_t contiguous_sequence) noexcept;
    std::uint64_t LastReceived() const noexcept { return receive_contiguous_; }
    std::uint64_t LastAcknowledged() const noexcept { return receive_committed_; }
    std::uint64_t PeerAcknowledged() const noexcept { return peer_acknowledged_; }
    std::uint64_t PeerOperationAcknowledged() const noexcept { return peer_operation_acknowledged_; }
    std::size_t InFlight() const noexcept { return outbound_.size(); }
    std::size_t BufferedOutOfOrder() const noexcept { return received_.size(); }
    void Reset(std::uint64_t session_generation) noexcept;
    std::uint64_t ReceiveOperationCommitted() const noexcept { return receive_operation_committed_; }
    std::uint64_t LastOperationAllocated() const noexcept { return last_operation_allocated_; }
    std::uint64_t SnapshotGeneration() const noexcept { return session_generation_; }

private:
    void ApplyPeerOperationAcknowledgement(std::uint64_t operation_sequence) noexcept;
    std::uint64_t session_generation_ = 1;
    std::uint64_t next_send_ = 1;
    std::uint64_t peer_acknowledged_ = 0;
    std::uint64_t peer_operation_acknowledged_ = 0;
    std::uint64_t receive_committed_ = 0;
    std::uint64_t receive_contiguous_ = 0;
    std::uint64_t last_operation_allocated_ = 0;
    std::uint64_t receive_operation_received_ = 0;
    std::uint64_t receive_operation_committed_ = 0;
    std::map<std::uint64_t, ControlMessage> outbound_;
    std::map<std::uint64_t, std::uint64_t> outbound_operations_;
    std::map<std::uint64_t, ControlMessage> received_;
    std::map<std::uint64_t, std::uint64_t> pending_commit_;
};

} // namespace ppp::app::multipath
