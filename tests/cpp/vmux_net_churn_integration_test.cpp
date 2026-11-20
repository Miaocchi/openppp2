#include <ppp/stdafx.h>
#include <ppp/app/mux/vmux_net.h>
#include <ppp/Random.h>
#include <ppp/configurations/AppConfiguration.h>

#include <iostream>
#include <stdexcept>

namespace vmux {

struct vmux_net_test_access {
    static void AttachEstablished(
        vmux_net& mux,
        const std::shared_ptr<ppp::app::mux::IMuxTransport>& transport,
        vmux_net::vmux_linklayer_ptr& link) {
        SynchronizationObjectScope scope(mux.syncobj_);
        if (!mux.attach_linklayer_locked(transport, link)) {
            throw std::runtime_error("carrier attach failed");
        }
        link->handshake_complete_ = true;
        mux.refresh_runtime_active_links();
    }

    static void MarkEstablished(vmux_net& mux) noexcept {
        mux.base_.established_ = true;
    }

    static void RecordStartupHandshake(vmux_net& mux) noexcept {
        ++mux.status_.opened_connections;
        mux.linklayer_established();
    }

    static void Sample(vmux_net& mux, uint64_t now, uint64_t acked, uint64_t offered) {
        mux.reliability_on_ = true;
        mux.tx_data_bytes_total_ = 16000;
        mux.pool_acked_payload_bytes_ = acked;
        mux.pool_offered_bytes_ = offered;
        mux.turbo_controller_tick(now);
    }

    static void AttachTrial(vmux_net& mux, const vmux_net::IMuxTransportPtr& transport) {
        AttachEstablished(mux, transport, mux.pool_trial_link_);
        // Make the original carrier the preferred idle victim of an untargeted shrink.
        mux.pool_trial_link_->queued_bytes_ = 1024;
    }

    static void Finalize(vmux_net& mux) noexcept {
        mux.finalize();
    }

    static std::pair<size_t, size_t> ContainerCounts(const vmux_net& mux) noexcept {
        return {mux.rx_links_.size(), mux.tx_links_.size()};
    }
};

} // namespace vmux

namespace {

using ppp::app::mux::IMuxTransport;
using ppp::threading::Executors;

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class FakeMuxTransport final : public IMuxTransport {
public:
    FakeMuxTransport(
        const ContextPtr& context,
        const StrandPtr& strand,
        const ppp::Int128& id)
        : context_(context), strand_(strand), id_(id) {}

    bool IsLinked() noexcept override { return linked_; }
    ContextPtr GetContext() noexcept override { return context_; }
    StrandPtr GetStrand() noexcept override { return strand_; }
    ITransmissionPtr GetTransmission() noexcept override { return nullptr; }
    ppp::Int128 GetId() noexcept override { return id_; }
    void Update() noexcept override {}

    void Dispose() noexcept override {
        if (linked_) {
            linked_ = false;
            ++disposals_;
        }
    }

    int disposals() const noexcept { return disposals_; }

private:
    ContextPtr context_;
    StrandPtr strand_;
    ppp::Int128 id_;
    bool linked_ = true;
    int disposals_ = 0;
};

} // namespace

int main() {
    try {
        ppp::Random overflow_seeded(INT_MAX);
        Require(overflow_seeded.Next() >= 0,
                "INT_MAX-seeded random generator returned a negative value");

        auto context = std::make_shared<boost::asio::io_context>();
        auto strand = std::make_shared<Executors::Strand>(context->get_executor());
        for (bool lazy : {false, true}) {
            auto startup = ppp::make_shared_object<vmux::vmux_net>(
                context, strand, 2, true, false);
            Require(static_cast<bool>(startup), "startup mux allocation failed");
            startup->AppConfiguration = ppp::make_shared_object<ppp::configurations::AppConfiguration>();
            Require(static_cast<bool>(startup->AppConfiguration), "startup configuration allocation failed");
            startup->set_pool_hard_max(4);
            if (lazy) startup->enable_lazy_pool();
            Require(startup->is_pool_lazy() == lazy, "incorrect pool negotiation state");
            Require(startup->get_pool_floor() == (lazy ? 1 : 2), "incorrect startup floor");
            Require(startup->get_pool_target() == startup->get_pool_floor(), "initial target must be the floor");
            Require(startup->get_max_connections() == 2, "pool negotiation changed the wire count");
            Require(startup->get_pool_hard_max() == (lazy ? 2 : 4), "incorrect negotiated ceiling");
            vmux::vmux_net_test_access::RecordStartupHandshake(*startup);
            Require(startup->is_established() == lazy, "incorrect first-handshake gate");
            if (!lazy) {
                vmux::vmux_net_test_access::RecordStartupHandshake(*startup);
                Require(startup->is_established(), "legacy session did not establish at its wire count");
            }
        }

        for (bool gain : {false, true}) {
            auto pool = ppp::make_shared_object<vmux::vmux_net>(context, strand, 2, false, false);
            pool->enable_lazy_pool();
            auto original = ppp::make_shared_object<FakeMuxTransport>(context, strand, ppp::Int128(1));
            auto probe = ppp::make_shared_object<FakeMuxTransport>(context, strand, ppp::Int128(2));
            vmux::vmux_net::vmux_linklayer_ptr link;
            using Access = vmux::vmux_net_test_access;
            Access::AttachEstablished(*pool, original, link);
            Access::MarkEstablished(*pool);
            uint64_t token = 0;
            unsigned calls = 0;
            pool->set_pool_grow_callback([&](uint64_t request) { token = request; ++calls; return true; });
            Access::Sample(*pool, 1000, 0, 0);
            Access::Sample(*pool, 3000, 30000, 40000);
            Access::Sample(*pool, 4000, 45000, 60000);
            Require(calls == 1 && token && pool->get_pool_target() == 1, "dial was duplicated or committed early");
            Access::AttachTrial(*pool, probe);
            Access::Sample(*pool, 5000, 60000, 80000);
            Access::Sample(*pool, 7000, 90000, 120000);
            Access::Sample(*pool, 9000, gain ? 129000 : 120000, 160000);
            Require(original->disposals() == 0 && probe->disposals() == (gain ? 0 : 1), "rollback retired wrong instance");
            Require(pool->get_pool_target() == (gain ? 2 : 1), "committed pool target ignored measured gain");
            vmux::vmux_net::vmux_native_add_linklayer_after_success_before_callback cb;
            Require(!pool->add_pool_trial_linklayer(probe, link, cb, token), "completed token accepted another carrier");
            Access::Finalize(*pool);
        }

        auto mux = ppp::make_shared_object<vmux::vmux_net>(
            context, strand, 1, true, false, vmux::vmux_net::mux_mode_compat);
        Require(static_cast<bool>(mux), "vmux allocation failed");
        mux->set_pool_hard_max(2);

        auto base_transport = ppp::make_shared_object<FakeMuxTransport>(
            context, strand, ppp::Int128(1));
        vmux::vmux_net::vmux_linklayer_ptr base_link;
        vmux::vmux_net_test_access::AttachEstablished(*mux, base_transport, base_link);
        vmux::vmux_net_test_access::MarkEstablished(*mux);
        // Keep the baseline carrier from being selected as the preferred idle
        // victim; each churn cycle is intended to drain the surplus carrier.
        base_link->queued_bytes_ = 64;
        Require(mux->get_runtime_state().active_links == 1,
                "base carrier was not published");
        Require(vmux::vmux_net_test_access::ContainerCounts(*mux) ==
                    std::make_pair<size_t, size_t>(1, 1),
                "base carrier containers were not initialized");

        for (int cycle = 0; cycle < 100; ++cycle) {
            auto extra_transport = ppp::make_shared_object<FakeMuxTransport>(
                context, strand, ppp::Int128(cycle + 2));
            vmux::vmux_net::vmux_linklayer_ptr extra_link;
            vmux::vmux_net_test_access::AttachEstablished(*mux, extra_transport, extra_link);
            Require(mux->get_runtime_state().active_links == 2,
                    "runtime carrier was not published");
            Require(vmux::vmux_net_test_access::ContainerCounts(*mux) ==
                        std::make_pair<size_t, size_t>(2, 2),
                    "runtime carrier containers did not grow");

            base_link->last_active_ = static_cast<uint64_t>(cycle + 2);
            extra_link->last_active_ = 0;
            const auto write = extra_link->drain_.BeginWrite();
            Require(static_cast<bool>(write), "runtime carrier rejected in-flight write");
            Require(mux->retire_linklayer_runtime(), "runtime carrier retire failed");
            Require(mux->get_runtime_state().active_links == 1,
                    "retiring carrier remained active");

            mux->reap_retired_linklayers();
            Require(extra_transport->disposals() == 0,
                    "carrier was disposed before its in-flight write drained");
            Require(extra_link->drain_.CompleteWrite(write),
                    "in-flight completion was not accepted");
            mux->reap_retired_linklayers();
            Require(extra_transport->disposals() == 1,
                    "drained carrier was not disposed exactly once");
            Require(mux->get_runtime_state().active_links == 1,
                    "base carrier count changed after reap");
            Require(vmux::vmux_net_test_access::ContainerCounts(*mux) ==
                        std::make_pair<size_t, size_t>(1, 1),
                    "retired carrier remained in scheduling containers");
        }

        vmux::vmux_net_test_access::Finalize(*mux);
        Require(base_transport->disposals() == 1,
                "base carrier was not disposed exactly once");
        std::cout << "vmux_net 100-cycle carrier container churn passed" << std::endl;
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}
