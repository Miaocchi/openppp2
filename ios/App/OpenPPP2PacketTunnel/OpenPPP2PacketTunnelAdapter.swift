import Darwin
import Foundation
import NetworkExtension
import OpenPPP2

final class OpenPPP2PacketTunnelAdapter {
    private static let packetSampleLimit = 16
    private static let packetTelemetryInitialLimit = 4
    private static let packetTelemetryInterval = 200
    private static let packetTelemetrySnapshotInterval: TimeInterval = 30
    private static let diagnosticsWriteInterval = 500
    private static let readBackpressureDelay: TimeInterval = 0.005
    private static let outputBatchLimit = 64
    private static let outputQueueHighWater = 384
    private static let outputQueueMax = 1024

    private let flow: NEPacketTunnelFlow
    private let packetFlowExporter: OTLPHTTPLogExporter?
    private var tap: OpaquePointer?
    private var isRunning = false
    private let statsQueue = DispatchQueue(label: "io.github.openppp2.packet-tunnel.stats")
    private let outputQueue = DispatchQueue(label: "io.github.openppp2.packet-tunnel.output", qos: .userInitiated)
    private let outputLock = NSLock()
    private var pendingOutputPackets: [(Data, NSNumber)] = []
    private var outputFlushScheduled = false
    private var outputDroppedCount = 0
    private var latestStatisticsJson = "{}"
    private var inputPacketCount = 0
    private var lastInputPacketSummary = ""
    private var inputPacketSamples: [String] = []
    private var outputPacketCount = 0
    private var lastOutputPacketSummary = ""
    private var lastOutputPacketOK = false
    private var outputPacketSamples: [String] = []
    private let diagnosticsFileURL: URL
    private var heartbeatTimer: DispatchSourceTimer?
    private var lastTelemetrySnapshotAt: Date?
    private var dataplane = "ctcp"

    init(flow: NEPacketTunnelFlow, telemetry: TelemetrySettings = .disabled) {
        self.flow = flow
        // Per-packet OTLP competes with the ~50 MiB NE budget; use native os_log + diagnostics.json.
        packetFlowExporter = nil
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
        diagnosticsFileURL = (documents ?? URL(fileURLWithPath: NSTemporaryDirectory()))
            .appendingPathComponent("openppp2-diagnostics.json")
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
        dataplane = options.lwip ? "lwip" : "ctcp"
        isRunning = true
        startHeartbeat()
        persistDiagnosticsSnapshot()
        readPackets()
        return true
    }

    func stop(stopReason: Int32 = -1) {
        isRunning = false
        stopHeartbeat()
        flushOutputPackets(force: true)

        if let tap {
            openppp2_ios_tap_stop(tap, stopReason)
            openppp2_ios_tap_destroy(tap)
            self.tap = nil
        }
        persistDiagnosticsSnapshot()
    }

    func telemetryStopAttributes() -> [String: OTLPAttributeValue] {
        outputLock.lock()
        let dropped = outputDroppedCount
        let pendingOutput = pendingOutputPackets.count
        outputLock.unlock()

        return [
            "openppp2.link_state": .int(Int64(linkState())),
            "openppp2.dataplane": .string(dataplane),
            "openppp2.input_packet_count": .int(Int64(latestInputPacketCount())),
            "openppp2.output_packet_count": .int(Int64(latestOutputPacketCount())),
            "openppp2.output_dropped_count": .int(Int64(dropped)),
            "openppp2.pending_output_depth": .int(Int64(pendingOutput)),
        ]
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

    func diagnosticsJson() -> String {
        let state = Int(linkState())
        let payload: [String: Any] = [
            "linkState": state,
            "startStage": startStage(),
            "lastError": openPPP2LastErrorText(),
            "inputPacketCount": latestInputPacketCount(),
            "lastInputPacket": latestInputPacketSummary(),
            "inputPackets": latestInputPacketSamples(),
            "outputPacketCount": latestOutputPacketCount(),
            "outputDroppedCount": latestOutputDroppedCount(),
            "lastOutputPacket": latestOutputPacketSummary(),
            "lastOutputPacketOK": latestOutputPacketOK(),
            "outputPackets": latestOutputPacketSamples(),
            "dataplane": dataplane,
            "statistics": statisticsJson()
        ]

        guard JSONSerialization.isValidJSONObject(payload),
              let data = try? JSONSerialization.data(withJSONObject: payload, options: [.sortedKeys]),
              let text = String(data: data, encoding: .utf8)
        else {
            return "{\"linkState\":\(state)}"
        }
        return text
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
                                    lwip: options.lwip ? 1 : 0,
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

    private func startStage() -> String {
        guard let tap else {
            return "tap not created"
        }

        var buffer = [CChar](repeating: 0, count: 256)
        let count = openppp2_ios_tap_get_start_stage(tap, &buffer, Int32(buffer.count))
        guard count > 0 else {
            return "stage unavailable"
        }
        return String(cString: buffer)
    }

    fileprivate func updateStatistics(_ json: UnsafePointer<CChar>?) {
        guard let json else { return }
        let text = String(cString: json)
        statsQueue.async { [weak self] in
            guard let self else { return }
            self.latestStatisticsJson = text
            self.writeDiagnosticsSnapshotLocked()
        }
    }

    private func readPackets() {
        guard isRunning else {
            return
        }

        if shouldPauseReadPackets() {
            DispatchQueue.global(qos: .userInitiated).asyncAfter(deadline: .now() + Self.readBackpressureDelay) { [weak self] in
                self?.readPackets()
            }
            return
        }

        flow.readPackets { [weak self] packets, protocols in
            guard let self else {
                return
            }

            if self.isRunning, let tap = self.tap {
                for (index, packet) in packets.enumerated() {
                    self.noteInputPacket(packet, protocolNumber: protocols.indices.contains(index) ? protocols[index] : nil)
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

    private func noteInputPacket(_ packet: Data, protocolNumber: NSNumber?) {
        statsQueue.async { [weak self] in
            guard let self else { return }
            self.inputPacketCount += 1
            let packetNumber = self.inputPacketCount
            let shouldSample = packetNumber <= Self.packetSampleLimit
                || packetNumber % Self.diagnosticsWriteInterval == 0
            let shouldLog = packetNumber <= 8
                || packetNumber % Self.diagnosticsWriteInterval == 0
            let shouldExport = self.shouldExportPacketTelemetry(packetNumber: packetNumber, ok: true)
            let shouldWrite = self.inputPacketCount <= 20
                || self.inputPacketCount % Self.diagnosticsWriteInterval == 0

            if shouldSample || shouldLog || shouldExport {
                let summary = Self.packetSummary(packet, protocolNumber: protocolNumber)
                self.lastInputPacketSummary = summary
                if shouldSample {
                    self.inputPacketSamples.append("#\(packetNumber) \(summary)")
                    if self.inputPacketSamples.count > Self.packetSampleLimit {
                        self.inputPacketSamples.removeFirst(self.inputPacketSamples.count - Self.packetSampleLimit)
                    }
                }
                if shouldLog {
                    NSLog("OpenPPP2 PacketTunnel input #%d %@", packetNumber, summary)
                }
                if shouldExport {
                    self.exportPacketTelemetryLocked(
                        direction: "input",
                        packetNumber: packetNumber,
                        packet: packet,
                        protocolNumber: protocolNumber,
                        summary: summary,
                        ok: true
                    )
                }
            }

            if shouldWrite {
                self.writeDiagnosticsSnapshotLocked()
            }
        }
    }

    private func latestInputPacketCount() -> Int {
        statsQueue.sync { inputPacketCount }
    }

    private func latestInputPacketSummary() -> String {
        statsQueue.sync { lastInputPacketSummary }
    }

    private func latestInputPacketSamples() -> [String] {
        statsQueue.sync { inputPacketSamples }
    }

    private func noteOutputPacket(_ packet: Data, protocolNumber: NSNumber, ok: Bool) {
        statsQueue.async { [weak self] in
            guard let self else { return }
            self.outputPacketCount += 1
            self.lastOutputPacketOK = ok
            let packetNumber = self.outputPacketCount
            let shouldSample = !ok
                || packetNumber <= Self.packetSampleLimit
                || packetNumber % Self.diagnosticsWriteInterval == 0
            let shouldLog = !ok
                || packetNumber <= 8
                || packetNumber % Self.diagnosticsWriteInterval == 0
            let shouldExport = self.shouldExportPacketTelemetry(packetNumber: packetNumber, ok: ok)
            let shouldWrite = self.outputPacketCount <= 20
                || self.outputPacketCount % Self.diagnosticsWriteInterval == 0
                || !ok

            if shouldSample || shouldLog || shouldExport {
                let summary = Self.packetSummary(packet, protocolNumber: protocolNumber)
                self.lastOutputPacketSummary = summary
                if shouldSample {
                    let okText = ok ? "ok" : "failed"
                    self.outputPacketSamples.append("#\(packetNumber) \(okText) \(summary)")
                    if self.outputPacketSamples.count > Self.packetSampleLimit {
                        self.outputPacketSamples.removeFirst(self.outputPacketSamples.count - Self.packetSampleLimit)
                    }
                }
                if shouldLog {
                    NSLog("OpenPPP2 PacketTunnel output #%d ok=%d %@", packetNumber, ok ? 1 : 0, summary)
                }
                if shouldExport {
                    self.exportPacketTelemetryLocked(
                        direction: "output",
                        packetNumber: packetNumber,
                        packet: packet,
                        protocolNumber: protocolNumber,
                        summary: summary,
                        ok: ok
                    )
                }
            }

            if shouldWrite {
                self.writeDiagnosticsSnapshotLocked()
            }
        }
    }

    private func latestOutputPacketCount() -> Int {
        statsQueue.sync { outputPacketCount }
    }

    private func latestOutputPacketSummary() -> String {
        statsQueue.sync { lastOutputPacketSummary }
    }

    private func latestOutputPacketOK() -> Bool {
        statsQueue.sync { lastOutputPacketOK }
    }

    private func latestOutputPacketSamples() -> [String] {
        statsQueue.sync { outputPacketSamples }
    }

    fileprivate func writePacket(_ packet: UnsafeRawPointer?, size: Int32) -> Int32 {
        guard let packet, size > 0 else {
            return 0
        }

        let data = Data(bytes: packet, count: Int(size))
        let protocolNumber = Self.protocolNumber(for: data)

        outputLock.lock()
        if pendingOutputPackets.count >= Self.outputQueueMax {
            outputDroppedCount += 1
            outputLock.unlock()
            return 0
        }
        pendingOutputPackets.append((data, protocolNumber))
        let depth = pendingOutputPackets.count
        let shouldFlushNow = depth >= Self.outputBatchLimit
        let scheduleFlush = !outputFlushScheduled && !shouldFlushNow
        if scheduleFlush {
            outputFlushScheduled = true
        }
        outputLock.unlock()

        if shouldFlushNow {
            outputQueue.async { [weak self] in
                self?.flushOutputPackets(force: false)
            }
        } else if scheduleFlush {
            outputQueue.asyncAfter(deadline: .now() + 0.001) { [weak self] in
                self?.flushOutputPackets(force: false)
            }
        }
        return 1
    }

    private func flushOutputPackets(force: Bool) {
        outputLock.lock()
        outputFlushScheduled = false
        guard !pendingOutputPackets.isEmpty else {
            outputLock.unlock()
            return
        }

        let takeCount = force
            ? pendingOutputPackets.count
            : min(Self.outputBatchLimit, pendingOutputPackets.count)
        let batch = Array(pendingOutputPackets.prefix(takeCount))
        pendingOutputPackets.removeFirst(takeCount)
        let morePending = !pendingOutputPackets.isEmpty
        if morePending && !outputFlushScheduled {
            outputFlushScheduled = true
        }
        outputLock.unlock()

        let ok = flow.writePackets(
            batch.map(\.0),
            withProtocols: batch.map(\.1)
        )
        if !ok {
            NSLog(
                "OpenPPP2 PacketTunnel writePackets batch failed count=%d bytes=%d",
                batch.count,
                batch.reduce(0) { $0 + $1.0.count }
            )
        }

        for (data, protocolNumber) in batch {
            noteOutputPacket(data, protocolNumber: protocolNumber, ok: ok)
        }

        if morePending {
            outputQueue.async { [weak self] in
                self?.flushOutputPackets(force: false)
            }
        }
    }

    private func latestOutputDroppedCount() -> Int {
        outputLock.lock()
        defer { outputLock.unlock() }
        return outputDroppedCount
    }

    private func pendingOutputDepth() -> Int {
        outputLock.lock()
        defer { outputLock.unlock() }
        return pendingOutputPackets.count
    }

    private func shouldPauseReadPackets() -> Bool {
        pendingOutputDepth() >= Self.outputQueueHighWater
    }

    private func persistDiagnosticsSnapshot() {
        statsQueue.async { [weak self] in
            self?.writeDiagnosticsSnapshotLocked()
        }
    }

    private func writeDiagnosticsSnapshotLocked() {
        outputLock.lock()
        let dropped = outputDroppedCount
        let pendingOutput = pendingOutputPackets.count
        outputLock.unlock()

        let payload: [String: Any] = [
            "linkState": Int(linkState()),
            "startStage": startStage(),
            "lastError": openPPP2LastErrorText(),
            "inputPacketCount": inputPacketCount,
            "lastInputPacket": lastInputPacketSummary,
            "inputPackets": inputPacketSamples,
            "outputPacketCount": outputPacketCount,
            "outputDroppedCount": dropped,
            "pendingOutputDepth": pendingOutput,
            "lastOutputPacket": lastOutputPacketSummary,
            "lastOutputPacketOK": lastOutputPacketOK,
            "outputPackets": outputPacketSamples,
            "dataplane": dataplane,
            "statistics": latestStatisticsJson,
            "updatedAt": ISO8601DateFormatter().string(from: Date())
        ]

        guard JSONSerialization.isValidJSONObject(payload),
              let data = try? JSONSerialization.data(withJSONObject: payload, options: [.prettyPrinted, .sortedKeys])
        else {
            return
        }

        try? data.write(to: diagnosticsFileURL, options: [.atomic])

        if let text = String(data: data, encoding: .utf8) {
            TunnelSharedState.writeLinkStateHeartbeat(Int(linkState()))
            TunnelSharedState.writeDiagnosticsJson(text)
        }
    }

    private func startHeartbeat() {
        stopHeartbeat()
        let timer = DispatchSource.makeTimerSource(queue: statsQueue)
        timer.schedule(deadline: .now(), repeating: 1.0)
        timer.setEventHandler { [weak self] in
            guard let self, self.isRunning else { return }
            TunnelSharedState.writeLinkStateHeartbeat(Int(self.linkState()))
            self.writeDiagnosticsSnapshotLocked()
            if self.shouldExportSnapshotTelemetryLocked() {
                self.exportSnapshotTelemetryLocked(reason: "heartbeat")
            }
        }
        timer.resume()
        heartbeatTimer = timer
    }

    private func stopHeartbeat() {
        heartbeatTimer?.cancel()
        heartbeatTimer = nil
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

    private func shouldExportPacketTelemetry(packetNumber: Int, ok: Bool) -> Bool {
        guard packetFlowExporter != nil else { return false }
        return !ok
            || packetNumber <= Self.packetTelemetryInitialLimit
            || packetNumber % Self.packetTelemetryInterval == 0
    }

    private func shouldExportSnapshotTelemetryLocked() -> Bool {
        guard packetFlowExporter != nil else { return false }
        let now = Date()
        if let lastTelemetrySnapshotAt,
           now.timeIntervalSince(lastTelemetrySnapshotAt) < Self.packetTelemetrySnapshotInterval {
            return false
        }
        lastTelemetrySnapshotAt = now
        return true
    }

    private func exportPacketTelemetryLocked(
        direction: String,
        packetNumber: Int,
        packet: Data,
        protocolNumber: NSNumber?,
        summary: String,
        ok: Bool
    ) {
        guard let packetFlowExporter else { return }

        let record = OTLPLogRecord(
            timeUnixNano: Self.unixNanoNow(),
            severityText: ok ? "INFO" : "ERROR",
            body: "packet_flow direction=\(direction) number=\(packetNumber) ok=\(ok ? 1 : 0) \(summary)",
            attributes: packetTelemetryAttributes(
                eventName: "openppp2.packet_flow.packet",
                direction: direction,
                packetNumber: packetNumber,
                packet: packet,
                protocolNumber: protocolNumber,
                summary: summary,
                ok: ok
            )
        )
        packetFlowExporter.export(records: [record]) { result in
            if case let .failure(error) = result {
                NSLog("OpenPPP2 packetFlow telemetry upload failed: %@", error.localizedDescription)
            }
        }
    }

    private func exportSnapshotTelemetryLocked(reason: String) {
        guard let packetFlowExporter else { return }

        let state = Int(linkState())
        let record = OTLPLogRecord(
            timeUnixNano: Self.unixNanoNow(),
            severityText: "INFO",
            body: "packet_flow snapshot reason=\(reason) linkState=\(state) input=\(inputPacketCount) output=\(outputPacketCount) lastOutputOK=\(lastOutputPacketOK ? 1 : 0)",
            attributes: [
                "event.name": .string("openppp2.packet_flow.snapshot"),
                "openppp2.packet_flow.reason": .string(reason),
                "openppp2.dataplane": .string(dataplane),
                "openppp2.link_state": .int(Int64(state)),
                "openppp2.packet.input_count": .int(Int64(inputPacketCount)),
                "openppp2.packet.output_count": .int(Int64(outputPacketCount)),
                "openppp2.packet.last_output_ok": .bool(lastOutputPacketOK),
                "openppp2.engine.statistics": .string(latestStatisticsJson)
            ]
        )
        packetFlowExporter.export(records: [record]) { result in
            if case let .failure(error) = result {
                NSLog("OpenPPP2 packetFlow snapshot upload failed: %@", error.localizedDescription)
            }
        }
    }

    private func packetTelemetryAttributes(
        eventName: String,
        direction: String,
        packetNumber: Int,
        packet: Data,
        protocolNumber: NSNumber?,
        summary: String,
        ok: Bool
    ) -> [String: OTLPAttributeValue] {
        var attributes: [String: OTLPAttributeValue] = [
            "event.name": .string(eventName),
            "openppp2.packet.direction": .string(direction),
            "openppp2.packet.number": .int(Int64(packetNumber)),
            "openppp2.packet.bytes": .int(Int64(packet.count)),
            "openppp2.packet.ok": .bool(ok),
            "openppp2.packet.summary": .string(summary),
            "openppp2.dataplane": .string(dataplane),
            "openppp2.packet.input_count": .int(Int64(inputPacketCount)),
            "openppp2.packet.output_count": .int(Int64(outputPacketCount)),
            "openppp2.link_state": .int(Int64(linkState()))
        ]
        attributes["openppp2.packet.af"] = .int(Int64(protocolNumber?.intValue ?? -1))
        if let version = packet.first.map({ Int($0 >> 4) }) {
            attributes["openppp2.packet.ip_version"] = .int(Int64(version))
        }
        if let transportProtocol = Self.transportProtocol(packet) {
            attributes["openppp2.packet.transport_protocol"] = .int(Int64(transportProtocol))
            attributes["openppp2.packet.transport_name"] = .string(Self.transportName(transportProtocol))
        }
        return attributes
    }

    private static func packetSummary(_ packet: Data, protocolNumber: NSNumber?) -> String {
        guard let first = packet.first else {
            return "empty packet"
        }

        let family = protocolNumber.map { "af=\($0.intValue)" } ?? "af=?"
        switch first >> 4 {
        case 4:
            return "\(family) \(ipv4Summary(packet))"
        case 6:
            return "\(family) \(ipv6Summary(packet))"
        default:
            return "\(family) unknown-version=\(first >> 4) len=\(packet.count)"
        }
    }

    private static func ipv4Summary(_ packet: Data) -> String {
        guard packet.count >= 20 else {
            return "ipv4 truncated len=\(packet.count)"
        }

        let headerLength = Int(packet[0] & 0x0f) * 4
        guard headerLength >= 20, packet.count >= headerLength else {
            return "ipv4 bad-header len=\(packet.count)"
        }

        let proto = Int(packet[9])
        let src = ipv4Address(packet, 12)
        let dst = ipv4Address(packet, 16)
        let ports = transportPorts(packet, offset: headerLength, proto: proto)
        let detail = transportDetail(packet, offset: headerLength, proto: proto)
        let checksum = ipv4ChecksumDetail(packet, headerLength: headerLength, proto: proto)
        return "ipv4 \(transportName(proto)) \(src)\(ports.src) -> \(dst)\(ports.dst) len=\(packet.count)\(detail)\(checksum)"
    }

    private static func ipv6Summary(_ packet: Data) -> String {
        guard packet.count >= 40 else {
            return "ipv6 truncated len=\(packet.count)"
        }

        let nextHeader = Int(packet[6])
        let src = ipv6Address(packet, 8)
        let dst = ipv6Address(packet, 24)
        let ports = transportPorts(packet, offset: 40, proto: nextHeader)
        let detail = transportDetail(packet, offset: 40, proto: nextHeader)
        return "ipv6 \(transportName(nextHeader)) \(src)\(ports.src) -> \(dst)\(ports.dst) len=\(packet.count)\(detail)"
    }

    private static func transportPorts(_ packet: Data, offset: Int, proto: Int) -> (src: String, dst: String) {
        guard (proto == 6 || proto == 17), packet.count >= offset + 4 else {
            return ("", "")
        }

        let src = (UInt16(packet[offset]) << 8) | UInt16(packet[offset + 1])
        let dst = (UInt16(packet[offset + 2]) << 8) | UInt16(packet[offset + 3])
        return (":\(src)", ":\(dst)")
    }

    private static func transportDetail(_ packet: Data, offset: Int, proto: Int) -> String {
        switch proto {
        case 6:
            guard packet.count >= offset + 20 else {
                return " tcp=truncated"
            }
            let dataOffset = Int(packet[offset + 12] >> 4) * 4
            guard dataOffset >= 20, packet.count >= offset + dataOffset else {
                return " tcp=bad-header"
            }
            let flags = Int(packet[offset + 13])
            let tcpPayloadLength = packet.count - offset - dataOffset
            let seq = uint32(packet, offset + 4)
            let ack = uint32(packet, offset + 8)
            let wnd = uint16(packet, offset + 14)
            return " flags=\(tcpFlags(flags)) seq=\(seq) ack=\(ack) wnd=\(wnd) tcpPayload=\(tcpPayloadLength)"
        case 17:
            guard packet.count >= offset + 8 else {
                return " udp=truncated"
            }
            let udpLength = Int(uint16(packet, offset + 4))
            let udpPayloadLength = max(0, udpLength - 8)
            return " udpPayload=\(udpPayloadLength)"
        default:
            return ""
        }
    }

    private static func tcpFlags(_ value: Int) -> String {
        var names: [String] = []
        if value & 0x01 != 0 { names.append("FIN") }
        if value & 0x02 != 0 { names.append("SYN") }
        if value & 0x04 != 0 { names.append("RST") }
        if value & 0x08 != 0 { names.append("PSH") }
        if value & 0x10 != 0 { names.append("ACK") }
        if value & 0x20 != 0 { names.append("URG") }
        if value & 0x40 != 0 { names.append("ECE") }
        if value & 0x80 != 0 { names.append("CWR") }
        return names.isEmpty ? "NONE" : names.joined(separator: "|")
    }

    private static func uint16(_ packet: Data, _ offset: Int) -> UInt16 {
        guard packet.count >= offset + 2 else { return 0 }
        return (UInt16(packet[offset]) << 8) | UInt16(packet[offset + 1])
    }

    private static func uint32(_ packet: Data, _ offset: Int) -> UInt32 {
        guard packet.count >= offset + 4 else { return 0 }
        return (UInt32(packet[offset]) << 24)
            | (UInt32(packet[offset + 1]) << 16)
            | (UInt32(packet[offset + 2]) << 8)
            | UInt32(packet[offset + 3])
    }

    private static func ipv4ChecksumDetail(_ packet: Data, headerLength: Int, proto: Int) -> String {
        let ipResidual = internetChecksum(packet, offset: 0, length: headerLength, seed: 0)
        var parts = [" ipCk=\(checksumText(ipResidual))"]

        if proto == 6, packet.count >= headerLength + 20 {
            let tcpLength = packet.count - headerLength
            var seed: UInt32 = 0
            seed += UInt32(uint16(packet, 12))
            seed += UInt32(uint16(packet, 14))
            seed += UInt32(uint16(packet, 16))
            seed += UInt32(uint16(packet, 18))
            seed += UInt32(proto)
            seed += UInt32(tcpLength)
            let tcpResidual = internetChecksum(packet, offset: headerLength, length: tcpLength, seed: seed)
            parts.append("tcpCk=\(checksumText(tcpResidual))")
        }

        return parts.joined(separator: " ")
    }

    private static func internetChecksum(_ packet: Data, offset: Int, length: Int, seed: UInt32) -> UInt16 {
        guard offset >= 0, length >= 0, packet.count >= offset + length else { return 0xffff }

        var sum = seed
        var index = offset
        let end = offset + length
        while index + 1 < end {
            sum += UInt32(UInt16(packet[index]) << 8 | UInt16(packet[index + 1]))
            index += 2
        }
        if index < end {
            sum += UInt32(UInt16(packet[index]) << 8)
        }

        while (sum >> 16) != 0 {
            sum = (sum & 0xffff) + (sum >> 16)
        }
        return UInt16(~sum & 0xffff)
    }

    private static func checksumText(_ value: UInt16) -> String {
        value == 0 ? "ok" : String(format: "bad:%04x", value)
    }

    private static func transportProtocol(_ packet: Data) -> Int? {
        guard let first = packet.first else { return nil }
        switch first >> 4 {
        case 4:
            guard packet.count >= 20 else { return nil }
            return Int(packet[9])
        case 6:
            guard packet.count >= 40 else { return nil }
            return Int(packet[6])
        default:
            return nil
        }
    }

    private static func unixNanoNow() -> UInt64 {
        UInt64(Date().timeIntervalSince1970 * 1_000_000_000)
    }

    private static func transportName(_ proto: Int) -> String {
        switch proto {
        case 1: return "ICMP"
        case 6: return "TCP"
        case 17: return "UDP"
        case 58: return "ICMPv6"
        default: return "proto\(proto)"
        }
    }

    private static func ipv4Address(_ packet: Data, _ offset: Int) -> String {
        guard packet.count >= offset + 4 else { return "?.?.?.?" }
        return "\(packet[offset]).\(packet[offset + 1]).\(packet[offset + 2]).\(packet[offset + 3])"
    }

    private static func ipv6Address(_ packet: Data, _ offset: Int) -> String {
        guard packet.count >= offset + 16 else { return "::" }
        var groups: [String] = []
        for index in stride(from: offset, to: offset + 16, by: 2) {
            let value = (UInt16(packet[index]) << 8) | UInt16(packet[index + 1])
            groups.append(String(value, radix: 16))
        }
        return groups.joined(separator: ":")
    }
}

func openPPP2LastErrorText() -> String {
    guard let error = openppp2_ios_last_error_text() else {
        return "Unknown OpenPPP2 error"
    }
    return String(cString: error)
}

struct PacketTunnelOptions: Codable {
    var tunIp: String = "10.8.0.2"
    var tunMask: String = "255.255.255.0"
    var tunPrefix: Int = 24
    var gateway: String = "10.8.0.1"
    var route: String = "0.0.0.0"
    var routePrefix: Int = 0
    var dns1: String = "8.8.8.8"
    var dns2: String = "1.1.1.1"
    var mtu: Int = 1400
    var mux: Int = 0
    var vnet: Bool = false
    var lwip: Bool = false
    var blockQuic: Bool = true
    var staticMode: Bool = true
    var bypassIpList: String = "10.0.0.0/8\n172.16.0.0/12\n192.168.0.0/16\n169.254.0.0/16\n100.64.0.0/10"
    var dnsRulesList: String = ""

    init() {}

    enum CodingKeys: String, CodingKey {
        case tunIp
        case tunMask
        case tunPrefix
        case gateway
        case route
        case routePrefix
        case dns1
        case dns2
        case mtu
        case mux
        case vnet
        case lwip
        case blockQuic
        case staticMode
        case bypassIpList
        case dnsRulesList
    }

    init(from decoder: Decoder) throws {
        let defaults = PacketTunnelOptions()
        let container = try decoder.container(keyedBy: CodingKeys.self)
        tunIp = try container.decodeIfPresent(String.self, forKey: .tunIp) ?? defaults.tunIp
        tunMask = try container.decodeIfPresent(String.self, forKey: .tunMask) ?? defaults.tunMask
        tunPrefix = try container.decodeIfPresent(Int.self, forKey: .tunPrefix) ?? defaults.tunPrefix
        gateway = try container.decodeIfPresent(String.self, forKey: .gateway) ?? defaults.gateway
        route = try container.decodeIfPresent(String.self, forKey: .route) ?? defaults.route
        routePrefix = try container.decodeIfPresent(Int.self, forKey: .routePrefix) ?? defaults.routePrefix
        dns1 = try container.decodeIfPresent(String.self, forKey: .dns1) ?? defaults.dns1
        dns2 = try container.decodeIfPresent(String.self, forKey: .dns2) ?? defaults.dns2
        mtu = try container.decodeIfPresent(Int.self, forKey: .mtu) ?? defaults.mtu
        mux = try container.decodeIfPresent(Int.self, forKey: .mux) ?? defaults.mux
        vnet = try container.decodeIfPresent(Bool.self, forKey: .vnet) ?? defaults.vnet
        lwip = try container.decodeIfPresent(Bool.self, forKey: .lwip) ?? defaults.lwip
        blockQuic = try container.decodeIfPresent(Bool.self, forKey: .blockQuic) ?? defaults.blockQuic
        staticMode = try container.decodeIfPresent(Bool.self, forKey: .staticMode) ?? defaults.staticMode
        bypassIpList = try container.decodeIfPresent(String.self, forKey: .bypassIpList) ?? defaults.bypassIpList
        dnsRulesList = try container.decodeIfPresent(String.self, forKey: .dnsRulesList) ?? defaults.dnsRulesList
    }
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
