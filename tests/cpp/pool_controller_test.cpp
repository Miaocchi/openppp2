#include <cstdlib>
#include <ppp/app/multipath/PoolController.h>

using namespace ppp::app::multipath;

static void check(bool condition) { if (!condition) std::abort(); }

static PoolSample start(PoolController& p, bool reliable = true) {
    PoolSample s;
    s.limit = 3;
    s.live = s.occupied = 1;
    s.pressure = true;
    s.reliable = reliable;
    check(p.Step(s) == PoolAction::Hold);
    s.now = 2000;
    s.offered = 40000;
    s.acked = reliable ? 30000 : 0;
    check(p.Step(s) == PoolAction::Dial);
    s.now += 2000;
    check(p.Step(s) == PoolAction::Hold); // dial claimed, but not handshaken
    return s;
}

static PoolAction measure(unsigned delivered, unsigned offered, unsigned delay) {
    PoolController p;
    auto s = start(p);
    ++s.topology;
    ++s.occupied;
    s.trial_attached = true;
    check(p.Step(s) == PoolAction::Hold);
    ++s.live;
    s.trial_ready = true;
    check(p.Step(s) == PoolAction::Hold);
    s.now += 2000;
    check(p.Step(s) == PoolAction::Hold); // exclude warm-up bytes
    s.now += 2000;
    s.acked += delivered;
    s.offered += offered;
    s.queue_delay_ms = delay;
    s.pressure = false;
    return p.Step(s);
}

int main() {
    check(measure(39000, 40000, 2) == PoolAction::Keep);
    check(measure(30000, 40000, 2) == PoolAction::Reject);
    check(measure(25000, 40000, 2) == PoolAction::Reject);
    check(measure(39000, 40000, 80) == PoolAction::Reject);
    check(measure(10000, 10000, 0) == PoolAction::Inconclusive);
    PoolController p;
    auto s = start(p);
    ++s.topology; // changed membership unrelated to the trial
    check(p.Step(s) == PoolAction::Inconclusive);
    PoolController q;
    s = start(q, false);
    ++s.topology;
    ++s.live;
    ++s.occupied;
    s.trial_ready = s.trial_attached = true;
    check(q.Step(s) == PoolAction::Keep); // explicit queue-only fallback
    PoolController r;
    s = start(r);
    s.now += 60000;
    check(r.Step(s) == PoolAction::Inconclusive);
    s.pressure = false;
    s.idle = true;
    ++s.live;
    ++s.occupied;
    s.now += 3000;
    check(r.Step(s) == PoolAction::Hold);
    s.now += 5000;
    check(r.Step(s) == PoolAction::Shrink);
    s.floor = s.live;
    s.now += 10000;
    check(r.Step(s) == PoolAction::Hold);

    PoolController tides;
    s = start(tides);
    s.reprobe_after_idle = true;
    ++s.topology;
    ++s.live;
    ++s.occupied;
    s.trial_ready = s.trial_attached = true;
    check(tides.Step(s) == PoolAction::Hold);
    s.now += 2000;
    check(tides.Step(s) == PoolAction::Hold);
    s.now += 2000;
    s.acked += 30000;
    s.offered += 40000;
    check(tides.Step(s) == PoolAction::Reject);
    s.trial_ready = s.trial_attached = false;
    s.now += 1000;
    s.pressure = false;
    s.idle = true;
    check(tides.Step(s) == PoolAction::Hold);
    s.now += 6000;
    check(tides.Step(s) == PoolAction::Shrink);
    ++s.topology;
    --s.live;
    --s.occupied;
    s.now += 1000;
    s.idle = false;
    s.pressure = true;
    check(tides.Step(s) == PoolAction::Hold);
    s.now += 2000;
    s.acked += 30000;
    s.offered += 40000;
    check(tides.Step(s) == PoolAction::Dial);
}
