#include <ppp/app/multipath/Control.h>

#include <algorithm>
#include <limits>

namespace ppp::app::multipath {

bool ReliableControlState::MakeMessage(std::uint64_t session_generation,
                                       std::uint64_t operation_sequence,
                                       const std::string& operation_id,
                                       const std::uint8_t* payload, std::size_t size,
                                       ControlMessage& output) noexcept {
    if (operation_id.empty() || operation_id.size() > MaxControlOperationId ||
        size > MaxControlPayload || (size != 0 && payload == nullptr) ||
        session_generation != session_generation_) return false;
    const auto retry = outbound_operations_.find(operation_sequence);
    if (retry != outbound_operations_.end()) {
        const auto existing = outbound_.find(retry->second);
        if (existing == outbound_.end()) return false;
        const ControlMessage& previous = existing->second;
        if (previous.operation_id != operation_id || previous.payload.size() != size ||
            (size != 0 && !std::equal(previous.payload.begin(), previous.payload.end(), payload))) return false;
        output = previous;
        return true;
    }
    if (operation_sequence == 0 || operation_sequence != last_operation_allocated_ + 1 ||
        outbound_.size() >= MaxControlWindow || next_send_ == std::numeric_limits<std::uint64_t>::max())
        return false;
    try {
        ControlMessage next;
        next.session_generation = session_generation_;
        next.sequence = next_send_;
        next.acknowledged = receive_committed_;
        next.operation_sequence = operation_sequence;
        next.acknowledged_operation_sequence = receive_operation_committed_;
        next.operation_id = operation_id;
        if (size != 0) next.payload.assign(payload, payload + size);
        if (!outbound_.emplace(next_send_, next).second) return false;
        try {
            if (!outbound_operations_.emplace(operation_sequence, next_send_).second) {
                outbound_.erase(next_send_);
                return false;
            }
        } catch (...) {
            outbound_.erase(next_send_);
            return false;
        }
        ++next_send_;
        last_operation_allocated_ = operation_sequence;
        output = std::move(next);
        return true;
    } catch (...) { return false; }
}

bool ReliableControlState::MakeAckOnly(ControlMessage& output) const noexcept {
    output = ControlMessage{};
    output.session_generation = session_generation_;
    output.acknowledged = receive_committed_;
    output.acknowledged_operation_sequence = receive_operation_committed_;
    output.acknowledgement_only = true;
    return true;
}

bool ReliableControlState::AdvanceGeneration(std::uint64_t new_generation,
    std::uint64_t peer_committed_operation_sequence,
    std::vector<ControlMessage>& replay) noexcept {
    if (new_generation <= session_generation_ || peer_committed_operation_sequence < peer_operation_acknowledged_ ||
        peer_committed_operation_sequence > last_operation_allocated_ ||
        !pending_commit_.empty() ||
        receive_operation_received_ != receive_operation_committed_) return false;

    std::map<std::uint64_t, ControlMessage> next_outbound;
    std::map<std::uint64_t, std::uint64_t> next_operations;
    std::vector<ControlMessage> next_replay;
    std::uint64_t next_sequence = 1;
    try {
        next_replay.reserve(outbound_.size());
        for (const auto& entry : outbound_) {
            const ControlMessage& previous = entry.second;
            if (previous.operation_sequence <= peer_committed_operation_sequence) continue;
            ControlMessage updated = previous;
            updated.session_generation = new_generation;
            updated.sequence = next_sequence++;
            updated.acknowledged = 0;
            updated.acknowledged_operation_sequence = receive_operation_committed_;
            next_outbound.emplace(updated.sequence, updated);
            next_operations.emplace(updated.operation_sequence, updated.sequence);
            next_replay.push_back(std::move(updated));
        }
    } catch (...) { return false; }

    session_generation_ = new_generation;
    next_send_ = next_sequence;
    peer_acknowledged_ = 0;
    peer_operation_acknowledged_ = peer_committed_operation_sequence;
    receive_committed_ = 0;
    receive_contiguous_ = 0;
    outbound_.swap(next_outbound);
    outbound_operations_.swap(next_operations);
    received_.clear();
    pending_commit_.clear();
    replay.swap(next_replay);
    return true;
}

bool ReliableControlState::Acknowledge(std::uint64_t sequence) noexcept {
    if (sequence < peer_acknowledged_ || sequence >= next_send_) return false;
    for (auto it = outbound_.begin(); it != outbound_.end() && it->first <= sequence;) {
        peer_operation_acknowledged_ = std::max(peer_operation_acknowledged_,
            it->second.operation_sequence);
        outbound_operations_.erase(it->second.operation_sequence);
        it = outbound_.erase(it);
    }
    peer_acknowledged_ = sequence;
    return true;
}

bool ReliableControlState::GetUnacknowledged(std::vector<ControlMessage>& output) const noexcept {
    try {
        output.clear();
        output.reserve(outbound_.size());
        for (const auto& item : outbound_) output.push_back(item.second);
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

ControlReceive ReliableControlState::Receive(const ControlMessage& message,
                                             std::vector<ControlDelivery>& ready) noexcept {
    ready.clear();
    if (message.session_generation != session_generation_) return ControlReceive::Invalid;
    if (message.acknowledgement_only) {
        if (message.sequence != 0 || message.operation_sequence != 0 ||
            !message.operation_id.empty() || !message.payload.empty() ||
            message.acknowledged >= next_send_ ||
            message.acknowledged_operation_sequence > last_operation_allocated_)
            return ControlReceive::Invalid;
        if (message.acknowledged > peer_acknowledged_) Acknowledge(message.acknowledged);
        ApplyPeerOperationAcknowledgement(message.acknowledged_operation_sequence);
        return ControlReceive::AckOnly;
    }
    if (message.sequence == 0 || message.operation_sequence == 0 || message.operation_id.empty() ||
        message.operation_id.size() > MaxControlOperationId ||
        message.payload.size() > MaxControlPayload || message.acknowledged >= next_send_ ||
        message.acknowledged_operation_sequence > last_operation_allocated_)
        return ControlReceive::Invalid;
    if (message.sequence <= receive_contiguous_ || received_.count(message.sequence) != 0) {
        if (message.acknowledged > peer_acknowledged_) Acknowledge(message.acknowledged);
        ApplyPeerOperationAcknowledgement(message.acknowledged_operation_sequence);
        return ControlReceive::Duplicate;
    }
    if (message.sequence - receive_contiguous_ > MaxControlWindow ||
        received_.size() + pending_commit_.size() >= MaxControlWindow)
        return ControlReceive::OutOfWindow;

    std::uint64_t simulated_operation_high_water = receive_operation_received_;
    for (std::uint64_t sequence = receive_contiguous_ + 1;; ++sequence) {
        const ControlMessage* next_message = nullptr;
        if (sequence == message.sequence) next_message = &message;
        else {
            const auto buffered = received_.find(sequence);
            if (buffered != received_.end()) next_message = &buffered->second;
        }
        if (next_message == nullptr) break;
        const std::uint64_t op_sequence = next_message->operation_sequence;
        if (op_sequence > simulated_operation_high_water) {
            if (op_sequence != simulated_operation_high_water + 1)
                return ControlReceive::Invalid;
            simulated_operation_high_water = op_sequence;
        }
        if (sequence == std::numeric_limits<std::uint64_t>::max()) break;
    }

    try {
        ready.reserve(MaxControlWindow);
        received_.emplace(message.sequence, message);
    } catch (...) { return ControlReceive::Capacity; }
    if (message.acknowledged > peer_acknowledged_) Acknowledge(message.acknowledged);
    ApplyPeerOperationAcknowledgement(message.acknowledged_operation_sequence);
    if (message.sequence != receive_contiguous_ + 1) return ControlReceive::Buffered;
    while (true) {
        auto next = received_.find(receive_contiguous_ + 1);
        if (next == received_.end()) break;
        const std::uint64_t op_sequence = next->second.operation_sequence;
        const bool duplicate_op = op_sequence <= receive_operation_received_;
        try {
            if (!pending_commit_.emplace(receive_contiguous_ + 1, op_sequence).second)
                return ControlReceive::Capacity;
        } catch (...) { return ControlReceive::Capacity; }
        if (!duplicate_op) receive_operation_received_ = op_sequence;
        ControlDelivery delivery;
        delivery.message = std::move(next->second);
        delivery.apply_operation = !duplicate_op;
        ready.push_back(std::move(delivery));
        received_.erase(next);
        ++receive_contiguous_;
    }
    return ready.empty() ? ControlReceive::Buffered : ControlReceive::Delivered;
}

bool ReliableControlState::CommitReceived(std::uint64_t sequence) noexcept {
    if (sequence < receive_committed_ || sequence > receive_contiguous_) return false;
    for (auto it = pending_commit_.begin(); it != pending_commit_.end() && it->first <= sequence;) {
        receive_operation_committed_ = std::max(receive_operation_committed_, it->second);
        it = pending_commit_.erase(it);
    }
    receive_committed_ = sequence;
    return true;
}

void ReliableControlState::Reset(std::uint64_t session_generation) noexcept {
    session_generation_ = session_generation;
    next_send_ = 1;
    peer_acknowledged_ = 0;
    peer_operation_acknowledged_ = 0;
    receive_committed_ = 0;
    receive_contiguous_ = 0;
    last_operation_allocated_ = 0;
    receive_operation_received_ = 0;
    receive_operation_committed_ = 0;
    outbound_.clear();
    outbound_operations_.clear();
    received_.clear();
    pending_commit_.clear();
}

void ReliableControlState::ApplyPeerOperationAcknowledgement(std::uint64_t sequence) noexcept {
    if (sequence <= peer_operation_acknowledged_) return;
    for (auto it = outbound_operations_.begin(); it != outbound_operations_.end() && it->first <= sequence;) {
        outbound_.erase(it->second);
        it = outbound_operations_.erase(it);
    }
    peer_operation_acknowledged_ = sequence;
}

} // namespace ppp::app::multipath
