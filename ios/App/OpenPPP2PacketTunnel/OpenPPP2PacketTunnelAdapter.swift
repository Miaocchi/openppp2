import Darwin
import Foundation
import NetworkExtension
import OpenPPP2

final class OpenPPP2PacketTunnelAdapter {
    private let flow: NEPacketTunnelFlow
    private var tap: OpaquePointer?
    private var isRunning = false
    private let statsQueue = DispatchQueue(label: "io.github.openppp2.packet-tunnel.stats")
    private var latestStatisticsJson = "{}"

    init(flow: NEPacketTunnelFlow) {
        self.flow = flow
    }

    func start(configJson: String, options: PacketTunnelOptions) -> Bool {
        guard tap == nil else {
            return true
        }

        let userData = Unmanaged.passUnretained(self).toOpaque()
        guard let createdTap = openppp2_ios_tap_create(openPPP2PacketWriter, userData) else {
            return false
        }

        guard startNativeTap(createdTap, configJson: configJson, options: options, userData: userData) else {
            openppp2_ios_tap_destroy(createdTap)
            return false
        }

        tap = createdTap
        isRunning = true
        readPackets()
        return true
    }

    func stop() {
        isRunning = false

        if let tap {
            openppp2_ios_tap_stop(tap)
            openppp2_ios_tap_destroy(tap)
            self.tap = nil
        }
    }

    func statisticsJson() -> String {
        if let tap {
            var buffer = [CChar](repeating: 0, count: 512)
            let count = openppp2_ios_tap_get_statistics(tap, &buffer, Int32(buffer.count))
            if count > 0 {
                let text = String(cString: buffer)
                statsQueue.sync {
                    latestStatisticsJson = text
                }
                return text
            }
        }
        return statsQueue.sync { latestStatisticsJson }
    }

    func linkState() -> Int32 {
        guard let tap else {
            return 2
        }
        return openppp2_ios_tap_get_link_state(tap)
    }

    private func startNativeTap(
        _ tap: OpaquePointer,
        configJson: String,
        options: PacketTunnelOptions,
        userData: UnsafeMutableRawPointer
    ) -> Bool {
        let rootPath = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first?.path ?? NSTemporaryDirectory()

        return configJson.withCString { configPtr in
            options.tunIp.withCString { ipPtr in
                options.tunMask.withCString { maskPtr in
                    options.bypassIpList.withCString { bypassPtr in
                        options.dnsRulesList.withCString { rulesPtr in
                            rootPath.withCString { rootPtr in
                                var nativeOptions = openppp2_ios_tunnel_options(
                                    mux: Int32(options.mux),
                                    vnet: options.vnet ? 1 : 0,
                                    block_quic: options.blockQuic ? 1 : 0,
                                    static_mode: options.staticMode ? 1 : 0,
                                    ip: ipPtr,
                                    mask: maskPtr,
                                    bypass_ip_list: bypassPtr,
                                    dns_rules_list: rulesPtr,
                                    root_path: rootPtr
                                )

                                let code = openppp2_ios_tap_start(
                                    tap,
                                    configPtr,
                                    &nativeOptions,
                                    openPPP2StatisticsWriter,
                                    userData
                                )
                                return code == 0
                            }
                        }
                    }
                }
            }
        }
    }

    fileprivate func updateStatistics(_ json: UnsafePointer<CChar>?) {
        guard let json else { return }
        let text = String(cString: json)
        statsQueue.async { [weak self] in
            self?.latestStatisticsJson = text
        }
    }

    private func readPackets() {
        guard isRunning else {
            return
        }

        flow.readPackets { [weak self] packets, _ in
            guard let self else {
                return
            }

            if self.isRunning, let tap = self.tap {
                for packet in packets {
                    packet.withUnsafeBytes { buffer in
                        guard let baseAddress = buffer.baseAddress, !buffer.isEmpty else {
                            return
                        }

                        _ = openppp2_ios_tap_input(tap, baseAddress, Int32(buffer.count))
                    }
                }
            }

            self.readPackets()
        }
    }

    fileprivate func writePacket(_ packet: UnsafeRawPointer?, size: Int32) -> Int32 {
        guard let packet, size > 0 else {
            return 0
        }

        let data = Data(bytes: packet, count: Int(size))
        let protocolNumber = Self.protocolNumber(for: data)
        return flow.writePackets([data], withProtocols: [protocolNumber]) ? 1 : 0
    }

    private static func protocolNumber(for packet: Data) -> NSNumber {
        guard let firstByte = packet.first else {
            return NSNumber(value: AF_INET)
        }

        switch firstByte >> 4 {
        case 6:
            return NSNumber(value: AF_INET6)
        default:
            return NSNumber(value: AF_INET)
        }
    }
}

func openPPP2LastErrorText() -> String {
    guard let error = openppp2_ios_last_error_text() else {
        return "Unknown OpenPPP2 error"
    }
    return String(cString: error)
}

struct PacketTunnelOptions: Codable {
    var tunIp: String = "10.0.0.2"
    var tunMask: String = "255.255.255.0"
    var tunPrefix: Int = 24
    var gateway: String = "10.0.0.1"
    var route: String = "0.0.0.0"
    var routePrefix: Int = 0
    var dns1: String = "8.8.8.8"
    var dns2: String = "1.1.1.1"
    var mtu: Int = 1400
    var mux: Int = 0
    var vnet: Bool = false
    var blockQuic: Bool = true
    var staticMode: Bool = false
    var bypassIpList: String = ""
    var dnsRulesList: String = ""
}

private func openPPP2PacketWriter(
    packet: UnsafeRawPointer?,
    packetSize: Int32,
    userData: UnsafeMutableRawPointer?
) -> Int32 {
    guard let userData else {
        return 0
    }

    let adapter = Unmanaged<OpenPPP2PacketTunnelAdapter>
        .fromOpaque(userData)
        .takeUnretainedValue()

    return adapter.writePacket(packet, size: packetSize)
}

private func openPPP2StatisticsWriter(
    _ statisticsJson: UnsafePointer<CChar>?,
    _ userData: UnsafeMutableRawPointer?
) {
    guard let userData else {
        return
    }

    let adapter = Unmanaged<OpenPPP2PacketTunnelAdapter>
        .fromOpaque(userData)
        .takeUnretainedValue()

    adapter.updateStatistics(statisticsJson)
}
