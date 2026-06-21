#include <ios/OpenPPP2PacketTunnelBridge.h>
#include <ios/ppp/tap/TapIos.h>
#include <ppp/app/client/VEthernetExchanger.h>
#include <ppp/app/client/VEthernetNetworkSwitcher.h>
#include <ppp/auxiliary/JsonAuxiliary.h>
#include <ppp/configurations/AppConfiguration.h>
#include <ppp/diagnostics/Error.h>
#include <ppp/IDisposable.h>
#include <ppp/net/Ipep.h>
#include <ppp/net/IPEndPoint.h>
#include <ppp/net/asio/vdns.h>
#include <ppp/threading/BufferswapAllocator.h>
#include <ppp/threading/Executors.h>
#include <ppp/transmissions/ITransmissionStatistics.h>

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <thread>

struct openppp2_ios_tap
{
    std::mutex                                                                  sync;
    std::condition_variable                                                     start_condition;
    std::shared_ptr<ppp::tap::TapIos>                                           tap;
    std::shared_ptr<ppp::configurations::AppConfiguration>                      configuration;
    std::shared_ptr<ppp::app::client::VEthernetNetworkSwitcher>                 client;
    std::shared_ptr<boost::asio::io_context>                                    context;
    std::shared_ptr<ppp::threading::BufferswapAllocator>                        allocator;
    ppp::transmissions::ITransmissionStatistics                                 statistics_reference;
    std::thread                                                                 runtime_thread;
    openppp2_ios_packet_writer                                                  writer = nullptr;
    void*                                                                       user_data = nullptr;
    ppp::string                                                                 latest_statistics = "{}";
    uint64_t                                                                    inbound_total = 0;
    uint64_t                                                                    outbound_total = 0;
    uint64_t                                                                    inbound_last = 0;
    uint64_t                                                                    outbound_last = 0;
    int                                                                         link_state = 6;
    int                                                                         start_result = 1;
    bool                                                                        start_completed = false;
    bool                                                                        running = false;
    bool                                                                        stopping = false;
};

namespace
{
    using ppp::app::client::VEthernetExchanger;
    using ppp::app::client::VEthernetNetworkSwitcher;
    using ppp::auxiliary::JsonAuxiliary;
    using ppp::configurations::AppConfiguration;
    using ppp::diagnostics::ErrorCode;
    using ppp::diagnostics::FormatErrorString;
    using ppp::diagnostics::GetLastErrorCodeSnapshot;
    using ppp::diagnostics::SetLastErrorCode;
    using ppp::net::IPEndPoint;
    using ppp::net::Ipep;
    using ppp::threading::BufferswapAllocator;
    using ppp::threading::Executors;
    using ppp::transmissions::ITransmissionStatistics;

    std::mutex g_last_error_mutex;
    ppp::string g_last_error_text = "success";

    void set_last_error(const char* text) noexcept
    {
        std::lock_guard<std::mutex> scope(g_last_error_mutex);
        g_last_error_text = text == nullptr ? "unknown" : text;
    }

    void set_last_error_from_diagnostics(const char* fallback) noexcept
    {
        ErrorCode code = GetLastErrorCodeSnapshot();
        const char* formatted = FormatErrorString(code);
        if (formatted != nullptr && std::strcmp(formatted, "success") != 0)
        {
            set_last_error(formatted);
        }
        else
        {
            set_last_error(fallback);
        }
    }

    bool copy_text(const ppp::string& source, char* buffer, int buffer_size) noexcept
    {
        if (buffer == nullptr || buffer_size < 1)
        {
            return false;
        }

        int count = std::min<int>(static_cast<int>(source.size()), buffer_size - 1);
        if (count > 0)
        {
            std::memcpy(buffer, source.data(), static_cast<size_t>(count));
        }
        buffer[count] = '\0';
        return true;
    }

    ppp::string make_statistics_json(uint64_t rx_speed, uint64_t tx_speed, uint64_t in_total, uint64_t out_total) noexcept
    {
        char buffer[192];
        std::snprintf(
            buffer,
            sizeof(buffer),
            "{\"rx\":\"%" PRIu64 "\",\"tx\":\"%" PRIu64 "\",\"in\":\"%" PRIu64 "\",\"out\":\"%" PRIu64 "\"}",
            rx_speed,
            tx_speed,
            in_total,
            out_total);
        return ppp::string(buffer);
    }

    ppp::string c_string_or_empty(const char* value) noexcept
    {
        return value == nullptr ? ppp::string() : ppp::string(value);
    }

    struct tunnel_options_snapshot
    {
        int         mux = 0;
        int         vnet = 0;
        int         block_quic = 0;
        int         static_mode = 0;
        ppp::string ip;
        ppp::string mask;
        ppp::string bypass_ip_list;
        ppp::string dns_rules_list;
        ppp::string root_path;
    };

    tunnel_options_snapshot snapshot_options(const openppp2_ios_tunnel_options& options) noexcept
    {
        tunnel_options_snapshot snapshot;
        snapshot.mux = options.mux;
        snapshot.vnet = options.vnet;
        snapshot.block_quic = options.block_quic;
        snapshot.static_mode = options.static_mode;
        snapshot.ip = c_string_or_empty(options.ip);
        snapshot.mask = c_string_or_empty(options.mask);
        snapshot.bypass_ip_list = c_string_or_empty(options.bypass_ip_list);
        snapshot.dns_rules_list = c_string_or_empty(options.dns_rules_list);
        snapshot.root_path = c_string_or_empty(options.root_path);
        return snapshot;
    }

    bool parse_ipv4(const char* text, boost::asio::ip::address& address, const char* error_text) noexcept
    {
        ppp::string value = c_string_or_empty(text);
        if (value.empty())
        {
            set_last_error(error_text);
            return false;
        }

        boost::system::error_code ec;
        address = ppp::StringToAddress(value.data(), ec);
        if (ec || !address.is_v4())
        {
            set_last_error(error_text);
            return false;
        }

        return true;
    }

    bool parse_configuration(const char* configuration_json, std::shared_ptr<AppConfiguration>& configuration) noexcept
    {
        if (configuration_json == nullptr || configuration_json[0] == '\0')
        {
            set_last_error("configuration json is empty");
            return false;
        }

        std::shared_ptr<AppConfiguration> parsed = ppp::make_shared_object<AppConfiguration>();
        if (parsed == nullptr)
        {
            set_last_error("failed to allocate app configuration");
            return false;
        }

        Json::Value json = JsonAuxiliary::FromString(configuration_json);
        if (!json.isObject())
        {
            set_last_error("configuration json is not an object");
            return false;
        }

        if (!parsed->Load(json))
        {
            set_last_error_from_diagnostics("failed to load app configuration");
            return false;
        }

        ppp::net::asio::vdns::ttl = parsed->udp.dns.ttl;
        ppp::net::asio::vdns::enabled = parsed->udp.dns.turbo;
        configuration = parsed;
        return true;
    }

    bool normalize_tunnel_addresses(
        const tunnel_options_snapshot& options,
        boost::asio::ip::address&      ip_address,
        boost::asio::ip::address&      mask_address,
        boost::asio::ip::address&      gateway_address,
        uint32_t&                      ip,
        uint32_t&                      mask,
        uint32_t&                      gateway) noexcept
    {
        if (!parse_ipv4(options.ip.c_str(), ip_address, "tunnel ip is invalid"))
        {
            return false;
        }

        if (!parse_ipv4(options.mask.c_str(), mask_address, "tunnel mask is invalid"))
        {
            return false;
        }

        ip = IPEndPoint::ToEndPoint(boost::asio::ip::tcp::endpoint(ip_address, IPEndPoint::MinPort)).GetAddress();
        mask = IPEndPoint::ToEndPoint(boost::asio::ip::tcp::endpoint(mask_address, IPEndPoint::MinPort)).GetAddress();
        if (ip == IPEndPoint::AnyAddress || ip == IPEndPoint::LoopbackAddress || ip == IPEndPoint::NoneAddress)
        {
            set_last_error("tunnel ip is unusable");
            return false;
        }

        int prefix = IPEndPoint::NetmaskToPrefix(mask);
        if (prefix < 16)
        {
            set_last_error("tunnel mask range is too large");
            return false;
        }

        if (prefix > 30)
        {
            mask = IPEndPoint::NetmaskToPrefix(prefix);
            mask_address = Ipep::ToAddress(mask);
        }

        if (IPEndPoint::IsInvalid(ip_address))
        {
            set_last_error("tunnel ip is invalid");
            return false;
        }

        gateway_address = Ipep::FixedIPAddress(ip_address, mask_address);
        ip_address = Ipep::FixedIPAddress(ip_address, gateway_address, mask_address);
        ip = IPEndPoint::ToEndPoint(boost::asio::ip::tcp::endpoint(ip_address, IPEndPoint::MinPort)).GetAddress();
        gateway = IPEndPoint::ToEndPoint(boost::asio::ip::tcp::endpoint(gateway_address, IPEndPoint::MinPort)).GetAddress();
        return true;
    }

    int map_link_state(const std::shared_ptr<VEthernetNetworkSwitcher>& client) noexcept
    {
        if (client == nullptr)
        {
            return 2;
        }

        std::shared_ptr<VEthernetExchanger> exchanger = client->GetExchanger();
        if (exchanger == nullptr)
        {
            return 3;
        }

        VEthernetExchanger::NetworkState state = exchanger->GetNetworkState();
        if (state == VEthernetExchanger::NetworkState_Connecting)
        {
            return 5;
        }

        if (state == VEthernetExchanger::NetworkState_Reconnecting)
        {
            return 4;
        }

        if (state == VEthernetExchanger::NetworkState_Established)
        {
            return 0;
        }

        return 1;
    }

    bool refresh_engine_statistics_locked(openppp2_ios_tap* tap) noexcept
    {
        if (tap == nullptr || tap->client == nullptr || tap->client->IsDisposed())
        {
            return false;
        }

        std::shared_ptr<ITransmissionStatistics> source = tap->client->GetStatistics();
        if (source == nullptr)
        {
            return false;
        }

        uint64_t incoming = 0;
        uint64_t outgoing = 0;
        std::shared_ptr<ITransmissionStatistics> snapshot;
        if (!ITransmissionStatistics::GetTransmissionStatistics(source, tap->statistics_reference, incoming, outgoing, snapshot))
        {
            return false;
        }

        uint64_t total_in = snapshot == nullptr ? 0 : snapshot->IncomingTraffic.load();
        uint64_t total_out = snapshot == nullptr ? 0 : snapshot->OutgoingTraffic.load();
        tap->latest_statistics = make_statistics_json(incoming, outgoing, total_in, total_out);
        return true;
    }

    void refresh_statistics(openppp2_ios_tap* tap, openppp2_ios_statistics_writer writer, void* writer_user_data) noexcept
    {
        if (nullptr == tap)
        {
            return;
        }

        uint64_t rx_speed = 0;
        uint64_t tx_speed = 0;
        {
            std::lock_guard<std::mutex> scope(tap->sync);
            if (!refresh_engine_statistics_locked(tap))
            {
                rx_speed = tap->inbound_total >= tap->inbound_last ? tap->inbound_total - tap->inbound_last : 0;
                tx_speed = tap->outbound_total >= tap->outbound_last ? tap->outbound_total - tap->outbound_last : 0;
                tap->inbound_last = tap->inbound_total;
                tap->outbound_last = tap->outbound_total;
                tap->latest_statistics = make_statistics_json(rx_speed, tx_speed, tap->inbound_total, tap->outbound_total);
            }
        }

        if (nullptr != writer)
        {
            writer(tap->latest_statistics.c_str(), writer_user_data);
        }
    }

    bool change_working_directory(const char* root_path) noexcept
    {
        if (root_path == nullptr || root_path[0] == '\0')
        {
            return true;
        }

        if (::chdir(root_path) == 0)
        {
            return true;
        }

        set_last_error("failed to switch OpenPPP2 root path");
        return false;
    }

    int complete_start(openppp2_ios_tap* tap, int result) noexcept
    {
        if (tap != nullptr)
        {
            {
                std::lock_guard<std::mutex> scope(tap->sync);
                if (!tap->start_completed)
                {
                    tap->start_result = result;
                    tap->start_completed = true;
                }
            }
            tap->start_condition.notify_all();
        }
        return result;
    }

    bool start_runtime(
        openppp2_ios_tap*                  tap,
        const char*                        configuration_json,
        const openppp2_ios_tunnel_options* options,
        openppp2_ios_statistics_writer     statistics_writer,
        void*                              statistics_user_data) noexcept
    {
        if (tap == nullptr || tap->writer == nullptr)
        {
            set_last_error("tap bridge is not initialized");
            return false;
        }

        if (options == nullptr)
        {
            set_last_error("tunnel options are null");
            return false;
        }

        tunnel_options_snapshot options_copy = snapshot_options(*options);

        if (!change_working_directory(options_copy.root_path.c_str()))
        {
            return false;
        }

        std::shared_ptr<AppConfiguration> configuration;
        if (!parse_configuration(configuration_json, configuration))
        {
            return false;
        }

        boost::asio::ip::address ip_address;
        boost::asio::ip::address mask_address;
        boost::asio::ip::address gateway_address;
        uint32_t ip = IPEndPoint::AnyAddress;
        uint32_t mask = IPEndPoint::AnyAddress;
        uint32_t gateway = IPEndPoint::AnyAddress;
        if (!normalize_tunnel_addresses(options_copy, ip_address, mask_address, gateway_address, ip, mask, gateway))
        {
            return false;
        }

        {
            std::lock_guard<std::mutex> scope(tap->sync);
            tap->start_completed = false;
            tap->start_result = 1;
            tap->stopping = false;
            tap->running = true;
            tap->link_state = 5;
            tap->configuration = configuration;
            tap->statistics_reference.Clear();
            tap->inbound_total = 0;
            tap->outbound_total = 0;
            tap->inbound_last = 0;
            tap->outbound_last = 0;
            tap->latest_statistics = "{}";
        }

        try
        {
            tap->runtime_thread = std::thread(
                [tap, configuration, options_copy, ip, gateway, mask, statistics_writer, statistics_user_data]() mutable noexcept
            {
                auto start = [tap, configuration, &options_copy, ip, gateway, mask, statistics_writer, statistics_user_data](int, const char**) noexcept -> int
                {
                    std::shared_ptr<boost::asio::io_context> context = Executors::GetDefault();
                    if (context == nullptr)
                    {
                        set_last_error("OpenPPP2 runtime context is unavailable");
                        return complete_start(tap, 1);
                    }

                    int max_concurrent = std::max<int>(1, ppp::GetProcesserCount());
                    Executors::SetMaxThreads(configuration->GetBufferAllocator(), max_concurrent);
                    Executors::SetMaxSchedulers(max_concurrent);

                    std::shared_ptr<ppp::tap::TapIos> ios_tap = ppp::tap::TapIos::Create(
                        context,
                        "packet-tunnel",
                        ip,
                        gateway,
                        mask,
                        false,
                        true,
                        ppp::vector<uint32_t>());

                    if (ios_tap == nullptr)
                    {
                        set_last_error("failed to create iOS packet tap");
                        return complete_start(tap, 1);
                    }

                    ios_tap->SetPacketOutput(
                        [tap](const void* packet, int packet_size) noexcept -> bool
                        {
                            bool ok = tap->writer(packet, packet_size, tap->user_data) != 0;
                            if (ok && packet_size > 0)
                            {
                                std::lock_guard<std::mutex> scope(tap->sync);
                                tap->outbound_total += static_cast<uint64_t>(packet_size);
                            }
                            return ok;
                        });

                    if (!ios_tap->Open())
                    {
                        set_last_error("failed to open iOS packet tap");
                        return complete_start(tap, 1);
                    }

                    bool lwip = false;
                    bool vnet = options_copy.vnet != 0;
                    bool mta = max_concurrent > 1;
                    std::shared_ptr<VEthernetNetworkSwitcher> client =
                        ppp::make_shared_object<VEthernetNetworkSwitcher>(context, lwip, vnet, mta, configuration);
                    if (client == nullptr)
                    {
                        set_last_error("failed to create OpenPPP2 client");
                        ios_tap->Dispose();
                        return complete_start(tap, 1);
                    }

                    uint16_t mux = static_cast<uint16_t>(std::min<int>(std::max<int>(0, options_copy.mux), UINT16_MAX));
                    bool static_mode = options_copy.static_mode != 0;
                    client->Mux(&mux);
                    client->StaticMode(&static_mode);
                    client->BlockQUIC(options_copy.block_quic != 0);

                    ppp::string dns_rules = options_copy.dns_rules_list;
                    if (!dns_rules.empty())
                    {
                        client->LoadAllDnsRules(dns_rules, false);
                    }

                    ppp::string bypass_ip_list = options_copy.bypass_ip_list;
                    if (!bypass_ip_list.empty())
                    {
                        client->SetBypassIpList(std::move(bypass_ip_list));
                    }

                    if (!client->Open(ios_tap))
                    {
                        set_last_error_from_diagnostics("failed to open OpenPPP2 client");
                        ppp::IDisposable::DisposeReferences(ios_tap, client);
                        return complete_start(tap, 1);
                    }

                    {
                        std::lock_guard<std::mutex> scope(tap->sync);
                        tap->tap = ios_tap;
                        tap->client = client;
                        tap->context = context;
                        tap->link_state = map_link_state(client);
                    }

                    refresh_statistics(tap, statistics_writer, statistics_user_data);
                    set_last_error("success");
                    return complete_start(tap, 0);
                };

                int rc = 1;
                try
                {
                    rc = Executors::Run(configuration->GetBufferAllocator(), start);
                }
                catch (const std::exception& e)
                {
                    set_last_error(e.what());
                    complete_start(tap, 1);
                    rc = 1;
                }
                catch (...)
                {
                    set_last_error("OpenPPP2 runtime failed");
                    complete_start(tap, 1);
                    rc = 1;
                }

                {
                    std::lock_guard<std::mutex> scope(tap->sync);
                    tap->running = false;
                    tap->context.reset();
                    if (tap->start_result != 0)
                    {
                        tap->tap.reset();
                        tap->client.reset();
                        tap->configuration.reset();
                    }
                    tap->link_state = tap->stopping ? 2 : tap->link_state;
                }
                tap->start_condition.notify_all();
                (void)rc;
            });
        }
        catch (const std::exception& e)
        {
            set_last_error(e.what());
            complete_start(tap, 1);
        }
        catch (...)
        {
            set_last_error("failed to start OpenPPP2 runtime thread");
            complete_start(tap, 1);
        }

        {
            std::unique_lock<std::mutex> lock(tap->sync);
            tap->start_condition.wait(lock, [tap]() noexcept { return tap->start_completed; });
            bool ok = tap->start_result == 0;
            lock.unlock();
            if (!ok && tap->runtime_thread.joinable())
            {
                tap->runtime_thread.join();
            }
            return ok;
        }
    }
}

const char* openppp2_ios_version(void)
{
    return "openppp2-ios";
}

openppp2_ios_tap* openppp2_ios_tap_create(
    openppp2_ios_packet_writer writer,
    void*                      user_data)
{
    if (nullptr == writer)
    {
        set_last_error("packet writer is null");
        return nullptr;
    }

    openppp2_ios_tap* bridge = new (std::nothrow) openppp2_ios_tap();
    if (nullptr == bridge)
    {
        set_last_error("failed to allocate tap bridge");
        return nullptr;
    }

    bridge->writer = writer;
    bridge->user_data = user_data;
    return bridge;
}

void openppp2_ios_tap_destroy(openppp2_ios_tap* tap)
{
    if (nullptr == tap)
    {
        return;
    }

    openppp2_ios_tap_stop(tap);
    delete tap;
}

int openppp2_ios_tap_start(
    openppp2_ios_tap*                  tap,
    const char*                        configuration_json,
    const openppp2_ios_tunnel_options* options,
    openppp2_ios_statistics_writer     statistics_writer,
    void*                              statistics_user_data)
{
    if (nullptr == tap || nullptr == tap->writer)
    {
        set_last_error("tap bridge is not initialized");
        return 1;
    }

    {
        std::lock_guard<std::mutex> scope(tap->sync);
        if (tap->running || nullptr != tap->tap || nullptr != tap->client)
        {
            return 0;
        }
    }

    if (!start_runtime(tap, configuration_json, options, statistics_writer, statistics_user_data))
    {
        return 1;
    }

    return 0;
}

int openppp2_ios_tap_stop(openppp2_ios_tap* tap)
{
    if (nullptr == tap)
    {
        return 0;
    }

    std::shared_ptr<VEthernetNetworkSwitcher> client;
    std::shared_ptr<ppp::tap::TapIos> ios_tap;
    std::shared_ptr<boost::asio::io_context> context;
    {
        std::lock_guard<std::mutex> scope(tap->sync);
        tap->stopping = true;
        client = tap->client;
        ios_tap = tap->tap;
        context = tap->context;
    }

    if (nullptr != client)
    {
        client->Dispose();
    }

    if (nullptr != ios_tap)
    {
        ios_tap->Dispose();
    }

    if (nullptr != context)
    {
        Executors::Exit(context);
    }
    else
    {
        Executors::Exit();
    }

    if (tap->runtime_thread.joinable())
    {
        tap->runtime_thread.join();
    }

    std::lock_guard<std::mutex> scope(tap->sync);
    tap->tap.reset();
    tap->client.reset();
    tap->context.reset();
    tap->configuration.reset();
    tap->running = false;
    tap->stopping = false;
    tap->link_state = 2;
    tap->latest_statistics = "{}";
    return 0;
}

int openppp2_ios_tap_input(
    openppp2_ios_tap* tap,
    const void*       packet,
    int               packet_size)
{
    if (nullptr == tap || nullptr == packet || packet_size < 1)
    {
        return 0;
    }

    std::shared_ptr<ppp::tap::TapIos> ios_tap;
    {
        std::lock_guard<std::mutex> scope(tap->sync);
        ios_tap = tap->tap;
    }

    if (nullptr == ios_tap)
    {
        return 0;
    }
    bool ok = ios_tap->Input(packet, packet_size);
    if (ok)
    {
        std::lock_guard<std::mutex> scope(tap->sync);
        tap->inbound_total += static_cast<uint64_t>(packet_size);
    }
    return ok ? 1 : 0;
}

int openppp2_ios_tap_get_link_state(openppp2_ios_tap* tap)
{
    if (nullptr == tap)
    {
        return 6;
    }

    std::lock_guard<std::mutex> scope(tap->sync);
    if (nullptr == tap->tap)
    {
        return 2;
    }

    tap->link_state = map_link_state(tap->client);
    return tap->link_state;
}

int openppp2_ios_tap_get_statistics(
    openppp2_ios_tap* tap,
    char*             buffer,
    int               buffer_size)
{
    if (nullptr == tap)
    {
        return 0;
    }

    ppp::string statistics;
    {
        std::lock_guard<std::mutex> scope(tap->sync);
        if (!refresh_engine_statistics_locked(tap))
        {
            uint64_t rx_speed = tap->inbound_total >= tap->inbound_last ? tap->inbound_total - tap->inbound_last : 0;
            uint64_t tx_speed = tap->outbound_total >= tap->outbound_last ? tap->outbound_total - tap->outbound_last : 0;
            tap->inbound_last = tap->inbound_total;
            tap->outbound_last = tap->outbound_total;
            tap->latest_statistics = make_statistics_json(rx_speed, tx_speed, tap->inbound_total, tap->outbound_total);
        }
        statistics = tap->latest_statistics;
    }

    if (!copy_text(statistics, buffer, buffer_size))
    {
        return 0;
    }
    return static_cast<int>(statistics.size());
}

const char* openppp2_ios_last_error_text(void)
{
    std::lock_guard<std::mutex> scope(g_last_error_mutex);
    return g_last_error_text.data();
}
