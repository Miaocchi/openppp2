#include <ppp/p2p/P2PProbeCoordinator.h>

#include <algorithm>
#include <limits>

namespace ppp::p2p {

bool P2PProbeCoordinator::Begin(P2PProbeRole role,
    const std::vector<P2PCandidateEndpoint>& local_candidates,
    const std::vector<P2PCandidateEndpoint>& peer_candidates,
    std::uint64_t now_ms,
    std::uint64_t generation) noexcept {
    if (local_candidates.empty() || peer_candidates.empty() ||
        now_ms > std::numeric_limits<std::uint64_t>::max() - SetupWindowMs ||
        (role != P2PProbeRole::Controlling && role != P2PProbeRole::Controlled)) {
        return false;
    }
    for (const auto& candidate : local_candidates) {
        if (!IsCanonicalP2PCandidate(candidate)) return false;
    }
    for (const auto& candidate : peer_candidates) {
        if (!IsCanonicalP2PCandidate(candidate)) return false;
    }

    std::array<P2PProbeCandidatePair, MaxPairs> pairs{};
    std::size_t count = 0;
    // Preserve the owner's candidate priority and ignore duplicate pairs.
    for (const auto& local : local_candidates) {
        for (const auto& peer : peer_candidates) {
            if (local.address_family != peer.address_family) continue;
            const P2PProbeCandidatePair pair{local, peer};
            const auto end = pairs.begin() + count;
            if (std::find(pairs.begin(), end, pair) == end) {
                if (count == MaxPairs) break;
                pairs[count++] = pair;
            }
        }
        if (count == MaxPairs) break;
    }
    if (count == 0) return false;

    active_ = true;
    timed_out_ = false;
    probing_started_ = true;
    cancelled_ = false;
    role_ = role;
    generation_ = generation;
    started_at_ms_ = now_ms;
    setup_deadline_ms_ = now_ms + SetupWindowMs;
    deadline_ms_ = now_ms + ProbeWindowMs;
    pairs_ = pairs;
    pair_count_ = count;
    emitted_rounds_ = 0;
    nominated_pair_.reset();
    acknowledged_pair_.reset();
    acknowledged_pairs_ = {};
    return true;
}

bool P2PProbeCoordinator::NominateResponderPair(
    const P2PProbeCandidatePair& pair,
    std::uint64_t now_ms,
    std::uint64_t generation) noexcept {
    if (cancelled_ || role_ != P2PProbeRole::Controlled ||
        generation != generation_ || now_ms < started_at_ms_ ||
        now_ms >= setup_deadline_ms_) return false;
    const auto index = FindPair(pair);
    if (!index) return false;
    if (nominated_pair_) {
        // Replayed authenticated nomination cannot extend the probe window.
        return *nominated_pair_ == *index;
    }
    if (now_ms >= deadline_ms_ && !acknowledged_pairs_[*index]) return false;
    nominated_pair_ = *index;
    if (acknowledged_pairs_[*index]) {
        acknowledged_pair_ = *index;
        active_ = false;
        timed_out_ = false;
    }
    return true;
}

P2PProbeBatch P2PProbeCoordinator::Poll(std::uint64_t now_ms,
    std::uint64_t generation) noexcept {
    P2PProbeBatch batch;
    if (!active_ || generation != generation_ || now_ms < started_at_ms_) {
        return batch;
    }
    if (now_ms >= deadline_ms_) {
        active_ = false;
        timed_out_ = true;
        return batch;
    }
    if (!probing_started_) return batch;

    // A delayed poll skips the missed round, rather than emitting two rounds
    // together. Normal polling sends at 0 and 2 seconds, bounded by 4 seconds.
    const std::uint8_t round = now_ms - started_at_ms_ >= RetryIntervalMs ? 2 : 1;
    if (round <= emitted_rounds_) return batch;
    emitted_rounds_ = round;
    if (role_ == P2PProbeRole::Controlled && nominated_pair_) {
        batch.tasks[batch.size++] = {pairs_[*nominated_pair_], *nominated_pair_, round};
    }
    else {
        for (std::size_t index = 0; index < pair_count_; ++index) {
            if (role_ == P2PProbeRole::Controlled && acknowledged_pairs_[index]) continue;
            batch.tasks[batch.size++] = {pairs_[index], index, round};
        }
    }
    return batch;
}

bool P2PProbeCoordinator::OnAuthenticatedAck(std::size_t pair_index,
    std::uint64_t now_ms,
    std::uint64_t generation) noexcept {
    if (!active_ || !probing_started_ || generation != generation_ ||
        now_ms < started_at_ms_ || now_ms >= deadline_ms_ ||
        pair_index >= pair_count_ || emitted_rounds_ == 0 ||
        (role_ == P2PProbeRole::Controlled && nominated_pair_ &&
            *nominated_pair_ != pair_index)) {
        return false;
    }
    if (acknowledged_pairs_[pair_index]) return false;
    acknowledged_pairs_[pair_index] = true;
    if (role_ == P2PProbeRole::Controlling || nominated_pair_) {
        acknowledged_pair_ = pair_index;
        active_ = false;
    }
    return true;
}

std::optional<std::size_t> P2PProbeCoordinator::FindPair(
    const P2PProbeCandidatePair& pair) const noexcept {
    for (std::size_t index = 0; index < pair_count_; ++index) {
        if (pairs_[index] == pair) return index;
    }
    return std::nullopt;
}

std::optional<P2PProbeCandidatePair> P2PProbeCoordinator::Pair(
    std::size_t pair_index) const noexcept {
    if (pair_index >= pair_count_) return std::nullopt;
    return pairs_[pair_index];
}

P2PProbeSnapshot P2PProbeCoordinator::Snapshot() const noexcept {
    return {active_, timed_out_, role_, generation_, deadline_ms_, pair_count_,
        emitted_rounds_, nominated_pair_, acknowledged_pair_, acknowledged_pairs_};
}

bool P2PProbeCoordinator::Cancel(std::uint64_t generation) noexcept {
    if (generation != generation_) return false;
    active_ = false;
    timed_out_ = false;
    probing_started_ = false;
    cancelled_ = true;
    pairs_ = {};
    pair_count_ = 0;
    emitted_rounds_ = 0;
    nominated_pair_.reset();
    acknowledged_pair_.reset();
    acknowledged_pairs_ = {};
    return true;
}

}
