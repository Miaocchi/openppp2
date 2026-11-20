#pragma once

#include <ppp/app/multipath/PathControl.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ppp::app::multipath {

// The adapter reports capability independently from the state machine.  This
// lets a platform expose a deliberate no-op while still distinguishing an
// operation that it cannot implement.
enum class PathAdapterResult : std::uint8_t {
    Applied,
    Noop,
    Unsupported,
    Rejected,
};

struct PathControlIntent final {
    PathEffectKind kind = PathEffectKind::PublishSnapshot;
    std::uint64_t value = 0;
    FailureClass failure = FailureClass::None;
    std::uint64_t epoch = 0;
    std::uint64_t generation = 0;
};

class IPathControlAdapter {
public:
    virtual ~IPathControlAdapter() = default;

    // Enqueue is the only state-machine input operation. Implementations must
    // not perform transport work on the caller's thread.
    virtual bool Enqueue(const PathEventEnvelope& event) noexcept = 0;
    virtual std::size_t Pump(std::size_t max_events,
                             std::vector<PathTransition>& transitions) noexcept = 0;
    virtual PathControlSnapshot Snapshot() const noexcept = 0;
    virtual void ResetEpoch(std::uint64_t epoch) noexcept = 0;

    // Optional capability gate. The default keeps existing adapters source
    // compatible; Apply remains authoritative and may still return Noop or
    // Unsupported when an operation is unavailable at runtime.
    virtual bool Supports(PathEffectKind) const noexcept { return true; }

    // Execute one state-machine effect against the underlying path. The
    // adapter does not own that path; Unsupported and Noop are valid results.
    virtual PathAdapterResult Apply(const PathControlIntent& intent) noexcept = 0;
};

// A non-owning facade used by schedulers and coordinators. It intentionally
// contains no transport lifetime or shutdown policy.
class PathAdapterFacade final {
public:
    explicit PathAdapterFacade(IPathControlAdapter& adapter) noexcept
        : adapter_(&adapter) {}

    bool Enqueue(const PathEventEnvelope& event) const noexcept {
        return adapter_ && adapter_->Enqueue(event);
    }
    std::size_t Pump(std::size_t max_events,
                     std::vector<PathTransition>& transitions) const noexcept {
        return adapter_ ? adapter_->Pump(max_events, transitions) : 0;
    }
    PathControlSnapshot Snapshot() const noexcept {
        return adapter_ ? adapter_->Snapshot() : PathControlSnapshot();
    }
    void ResetEpoch(std::uint64_t epoch) const noexcept {
        if (adapter_) adapter_->ResetEpoch(epoch);
    }
    bool Supports(PathEffectKind kind) const noexcept {
        return adapter_ && adapter_->Supports(kind);
    }
    PathAdapterResult Apply(const PathControlIntent& intent) const noexcept {
        return adapter_ ? adapter_->Apply(intent) : PathAdapterResult::Unsupported;
    }

private:
    IPathControlAdapter* adapter_;
};

/**
 * Default adapter for the pure state machine. It owns the control owner but
 * deliberately leaves transport effects unsupported until a backend adapter
 * supplies them.
 */
class PathControlAdapter final : public IPathControlAdapter {
public:
    explicit PathControlAdapter(std::string path_id = {}) noexcept
        : owner_(std::move(path_id)) {}

    bool Enqueue(const PathEventEnvelope& event) noexcept override {
        return owner_.Enqueue(event);
    }

    std::size_t Pump(std::size_t max_events,
                     std::vector<PathTransition>& transitions) noexcept override {
        return owner_.Pump(max_events, transitions);
    }

    PathControlSnapshot Snapshot() const noexcept override {
        return owner_.Snapshot();
    }

    void ResetEpoch(std::uint64_t epoch) noexcept override {
        owner_.ResetEpoch(epoch);
    }

    bool Supports(PathEffectKind kind) const noexcept override {
        return kind == PathEffectKind::PublishSnapshot;
    }

    PathAdapterResult Apply(const PathControlIntent& intent) noexcept override {
        const PathControlSnapshot snapshot = owner_.Snapshot();
        if (intent.epoch != snapshot.epoch || intent.generation != snapshot.generation) {
            return PathAdapterResult::Rejected;
        }
        return intent.kind == PathEffectKind::PublishSnapshot
            ? PathAdapterResult::Noop : PathAdapterResult::Unsupported;
    }

private:
    PathControlOwner owner_;
};

} // namespace ppp::app::multipath
