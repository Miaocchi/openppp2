#include <ppp/app/mux/TcpIntentClassifier.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace ppp::app::mux {

TcpIntentClassifier::TcpIntentClassifier(TcpIntentConfig config) noexcept
    : config_(std::move(config)) {
    if (config_.window_ms == 0) config_.window_ms = 250;
    if (config_.min_samples == 0) config_.min_samples = 16 * 1024;
    if (!std::isfinite(config_.hysteresis) || config_.hysteresis < 0.0) {
        config_.hysteresis = 0.20;
    }
    if (config_.hysteresis > 1.0) config_.hysteresis = 1.0;
}

void TcpIntentClassifier::Reset() noexcept {
    result_ = TcpIntentResult{};
    has_first_tick_ = false;
    first_tick_ = 0;
    last_tick_ = 0;
    bytes_ = 0;
    outbound_bytes_ = 0;
    inbound_bytes_ = 0;
    chunk_bytes_ = 0;
    max_chunk_bytes_ = 0;
    events_ = 0;
}

std::string TcpIntentClassifier::Lower(std::string value) noexcept {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

bool TcpIntentClassifier::Matches(const TcpIntentRule& rule,
                                  const std::string& host,
                                  std::uint16_t port) noexcept {
    if (rule.intent == TcpIntent::Unknown) return false;
    const bool has_port = rule.port_min != 0 || rule.port_max != 0;
    if (has_port) {
        const std::uint16_t maximum = rule.port_max == 0 ? rule.port_min : rule.port_max;
        const std::uint16_t minimum = rule.port_min == 0 ? maximum : rule.port_min;
        if (port < minimum || port > maximum) return false;
    }
    if (!rule.host_suffix.empty()) {
        const std::string candidate = Lower(host);
        const std::string suffix = Lower(rule.host_suffix);
        if (candidate.size() < suffix.size() ||
            candidate.compare(candidate.size() - suffix.size(), suffix.size(), suffix) != 0) {
            return false;
        }
    }
    return true;
}

bool TcpIntentClassifier::Seed(const std::string& host, std::uint16_t port,
                               const std::vector<TcpIntentRule>& rules) noexcept {
    if (!config_.enabled || config_.mode == "behavior" || result_.intent != TcpIntent::Unknown) {
        return false;
    }
    for (const TcpIntentRule& rule : rules) {
        if (!Matches(rule, host, port)) continue;
        result_.intent = rule.intent;
        result_.source = TcpIntentSource::Rule;
        result_.confidence = 1.0;
        result_.changed = true;
        return true;
    }
    return false;
}

double TcpIntentClassifier::Clamp01(double value) noexcept {
    if (value < 0.0) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
}

void TcpIntentClassifier::Evaluate(std::uint64_t now_ms) noexcept {
    if (!config_.enabled || result_.intent != TcpIntent::Unknown ||
        config_.mode == "rules" || !has_first_tick_) {
        return;
    }
    const std::uint64_t elapsed = now_ms >= first_tick_ ? now_ms - first_tick_ : 0;
    result_.sample_bytes = bytes_;
    result_.sample_duration_ms = elapsed;

    // A first full-size application write is a strong bandwidth signal. It
    // lets a bulk or media stream leave the unknown pool before the normal
    // time/volume window, while phase-shift traffic that starts with several
    // small exchanges still gets classified from its initial behavior.
    if (max_chunk_bytes_ >= 12 * 1024 && bytes_ <= 256 * 1024) {
        result_.intent = TcpIntent::Bandwidth;
        result_.source = TcpIntentSource::Behavior;
        result_.confidence = 0.75;
        result_.changed = true;
        return;
    }
    // A connection that only emits small application chunks is interactive
    // before it reaches the normal bulk sampling window. Classify after a few
    // kilobytes so the latency policy can protect request streams while
    // leaving 8 KiB gray-zone bursts unresolved.
    if (max_chunk_bytes_ <= 4 * 1024 && bytes_ >= 4 * 1024 && events_ >= 3) {
        result_.intent = TcpIntent::Latency;
        result_.source = TcpIntentSource::Behavior;
        result_.confidence = 0.75;
        result_.changed = true;
        return;
    }
    if (bytes_ < config_.min_samples) return;
    if (elapsed < config_.window_ms || elapsed == 0) return;

    const double seconds = static_cast<double>(elapsed) / 1000.0;
    const double rate = static_cast<double>(bytes_) / seconds;
    // Sparse medium writes provide neither the small-message signal of an
    // interactive exchange nor the sustained delivery signal of a bulk flow.
    if (max_chunk_bytes_ > 4 * 1024 && max_chunk_bytes_ < 12 * 1024 && rate < 128 * 1024) {
        return;
    }
    const double rate_score = Clamp01(rate / static_cast<double>(1024 * 1024));
    const double chunk_score = Clamp01(
        static_cast<double>(chunk_bytes_) / static_cast<double>(events_ * 32 * 1024));
    const double volume_score = Clamp01(
        static_cast<double>(bytes_) / static_cast<double>(256 * 1024));
    const std::uint64_t smaller = std::min(outbound_bytes_, inbound_bytes_);
    const std::uint64_t larger = std::max(outbound_bytes_, inbound_bytes_);
    const double duplex_score = larger == 0 ? 0.0 :
        Clamp01(static_cast<double>(smaller) / static_cast<double>(larger));

    const double latency = 0.35 * (1.0 - rate_score) +
        0.25 * (1.0 - chunk_score) + 0.20 * (1.0 - volume_score) +
        0.20 * duplex_score;
    const double bandwidth = 0.50 * rate_score + 0.25 * chunk_score +
        0.20 * volume_score + 0.05 * (1.0 - duplex_score);
    const double difference = std::fabs(latency - bandwidth);
    const double required_difference = std::max(0.10, config_.hysteresis * 0.5);
    if (difference < required_difference) return;

    result_.intent = latency > bandwidth ? TcpIntent::Latency : TcpIntent::Bandwidth;
    result_.source = TcpIntentSource::Behavior;
    result_.confidence = Clamp01(0.5 + difference * 0.5);
    result_.changed = true;
}

TcpIntentResult TcpIntentClassifier::Observe(std::uint64_t now_ms, std::size_t bytes,
                                             std::size_t chunk_bytes,
                                             bool outbound) noexcept {
    result_.changed = false;
    if (!config_.enabled || result_.intent != TcpIntent::Unknown) return result_;
    if (!has_first_tick_) {
        first_tick_ = now_ms;
        has_first_tick_ = true;
    }
    last_tick_ = now_ms;
    bytes_ += static_cast<std::uint64_t>(bytes);
    chunk_bytes_ += static_cast<std::uint64_t>(chunk_bytes);
    events_++;
    max_chunk_bytes_ = std::max(max_chunk_bytes_, static_cast<std::uint64_t>(chunk_bytes));
    if (outbound) outbound_bytes_ += static_cast<std::uint64_t>(bytes);
    else inbound_bytes_ += static_cast<std::uint64_t>(bytes);
    result_.sample_bytes = bytes_;
    result_.sample_duration_ms = now_ms >= first_tick_ ? now_ms - first_tick_ : 0;
    Evaluate(now_ms);
    return result_;
}

} // namespace ppp::app::mux
