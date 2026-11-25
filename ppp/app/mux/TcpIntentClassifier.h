#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ppp::app::mux {

/** Coarse application intent used by the VMUX path policy. */
enum class TcpIntent : std::uint8_t {
    Unknown = 0,
    Latency = 1,
    Bandwidth = 2,
};

enum class TcpIntentSource : std::uint8_t {
    Default = 0,
    Rule = 1,
    Behavior = 2,
};

struct TcpIntentRule final {
    TcpIntent intent = TcpIntent::Unknown;
    std::uint16_t port_min = 0;
    std::uint16_t port_max = 0;
    std::string host_suffix;
};

struct TcpIntentConfig final {
    bool enabled = false;
    std::string mode = "auto";
    std::uint64_t window_ms = 250;
    std::size_t min_samples = 16 * 1024;
    double hysteresis = 0.20;
    std::uint64_t hold_ms = 5000;
};

struct TcpIntentResult final {
    TcpIntent intent = TcpIntent::Unknown;
    TcpIntentSource source = TcpIntentSource::Default;
    double confidence = 0.0;
    std::uint64_t sample_bytes = 0;
    std::uint64_t sample_duration_ms = 0;
    bool changed = false;
};

/**
 * Bounded, strand-affine classifier for one logical TCP stream.
 *
 * The classifier never inspects payload contents. It uses a destination rule
 * when supplied and otherwise aggregates bounded early-stream timing and size
 * observations. Once a stream is classified, the result remains sticky for
 * the lifetime of this object.
 */
class TcpIntentClassifier final {
public:
    explicit TcpIntentClassifier(TcpIntentConfig config = {}) noexcept;

    void Reset() noexcept;

    /** Apply the first matching destination rule, if rule classification is enabled. */
    bool Seed(const std::string& host, std::uint16_t port,
              const std::vector<TcpIntentRule>& rules) noexcept;

    /** Add one observed stream read/write event. */
    TcpIntentResult Observe(std::uint64_t now_ms, std::size_t bytes,
                            std::size_t chunk_bytes, bool outbound) noexcept;

    TcpIntent Intent() const noexcept { return result_.intent; }
    const TcpIntentResult& Result() const noexcept { return result_; }
    const TcpIntentConfig& Config() const noexcept { return config_; }

    static bool Matches(const TcpIntentRule& rule, const std::string& host,
                        std::uint16_t port) noexcept;

private:
    static double Clamp01(double value) noexcept;
    static std::string Lower(std::string value) noexcept;
    void Evaluate(std::uint64_t now_ms) noexcept;

    TcpIntentConfig config_;
    TcpIntentResult result_;
    bool has_first_tick_ = false;
    std::uint64_t first_tick_ = 0;
    std::uint64_t last_tick_ = 0;
    std::uint64_t bytes_ = 0;
    std::uint64_t outbound_bytes_ = 0;
    std::uint64_t inbound_bytes_ = 0;
    std::uint64_t chunk_bytes_ = 0;
    std::uint64_t max_chunk_bytes_ = 0;
    std::uint64_t events_ = 0;
};

} // namespace ppp::app::mux
