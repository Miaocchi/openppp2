#include <cassert>
#include <ppp/app/multipath/PoolController.h>

using namespace ppp::app::multipath;

int main() {
    PoolDemand demand{1000, 1, 0, true};
    PoolCapacity capacity{1000, 0, false, 0, 100, 1, 4, 8};
    auto r = PoolController::Decide(demand, capacity, 10);
    assert(r.decision == PoolDecision::Hold); // all_at_capacity is not expansion.

    capacity.aggregate_rate_bytes_per_second = 500;
    capacity.marginal_gain_bytes_per_second = 150;
    capacity.carrier_pressure = true;
    r = PoolController::Decide(demand, capacity, 10);
    assert(r.decision == PoolDecision::Add);
    assert(r.single_carrier_pressure && r.aggregate_capacity_shortage);

    demand.warming_carriers = 1;
    capacity.warmup_until_ms = 1000;
    r = PoolController::Decide(demand, capacity, 10);
    assert(r.decision == PoolDecision::Hold && r.in_warmup);

    demand = PoolDemand{500, 3, 0, false};
    capacity = PoolCapacity{1000, 0, false, 0, 100, 1, 2, 4};
    r = PoolController::Decide(demand, capacity, 10);
    assert(r.decision == PoolDecision::Retire);

    demand = PoolDemand{0, 1, 0, false};
    capacity = PoolCapacity{1000000, 1, false, 0, 1, 1, 4};
    capacity.aggregate_capacity_shortage = true;
    r = PoolController::Decide(demand, capacity, 10);
    assert(r.decision == PoolDecision::Add);
    capacity.aggregate_capacity_shortage = false;
    r = PoolController::Decide(demand, capacity, 10);
    assert(r.decision == PoolDecision::Hold);
    return 0;
}
