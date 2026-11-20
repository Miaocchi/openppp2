import NetworkExtension
import QuartzCore
import UIKit

// MARK: - Localization

enum AppLanguage: String, CaseIterable {
    case system
    case zhHans
    case en

    var displayName: String {
        switch self {
        case .system: return L10n.tr("language.system")
        case .zhHans: return "简体中文"
        case .en: return "English"
        }
    }

    var detailText: String {
        switch self {
        case .system: return L10n.tr("language.system.detail")
        case .zhHans: return L10n.tr("language.zh.detail")
        case .en: return L10n.tr("language.en.detail")
        }
    }

    var resolvedCode: String {
        switch self {
        case .zhHans:
            return "zh-Hans"
        case .en:
            return "en"
        case .system:
            let preferred = Locale.preferredLanguages.first?.lowercased() ?? ""
            return preferred.hasPrefix("zh") ? "zh-Hans" : "en"
        }
    }
}

final class AppLanguageStore {
    static let shared = AppLanguageStore()
    static let didChangeNotification = Notification.Name("OpenPPP2AppLanguageDidChange")
    private let key = "openppp2_app_language"
    private let defaults = UserDefaults.standard

    private init() {}

    var language: AppLanguage {
        get {
            guard let raw = defaults.string(forKey: key),
                  let language = AppLanguage(rawValue: raw)
            else {
                return .system
            }
            return language
        }
        set {
            let oldLanguage = language
            let oldCode = oldLanguage.resolvedCode
            defaults.set(newValue.rawValue, forKey: key)
            if oldCode != newValue.resolvedCode || oldLanguage != newValue {
                NotificationCenter.default.post(name: Self.didChangeNotification, object: self)
            }
        }
    }

    var isChinese: Bool {
        language.resolvedCode.hasPrefix("zh")
    }

    var locale: Locale {
        Locale(identifier: isChinese ? "zh-Hans" : "en")
    }
}

enum L10n {
    static func tr(_ key: String) -> String {
        guard let value = strings[key] else {
            return key
        }
        return AppLanguageStore.shared.isChinese ? value.zh : value.en
    }

    static func format(_ key: String, _ args: CVarArg...) -> String {
        String(format: tr(key), locale: AppLanguageStore.shared.locale, arguments: args)
    }

    private static let strings: [String: (zh: String, en: String)] = [
        "tab.home": ("主页", "Home"),
        "tab.options": ("启动参数", "Launch"),
        "tab.profiles": ("配置文件", "Profiles"),
        "tab.settings": ("设置", "Settings"),

        "language.title": ("语言", "Language"),
        "language.system": ("跟随系统", "System"),
        "language.system.detail": ("根据 iOS 当前语言自动选择", "Follow the current iOS language"),
        "language.zh.detail": ("使用简体中文界面", "Use Simplified Chinese"),
        "language.en.detail": ("使用英文界面", "Use English"),
        "language.current": ("当前语言", "Current language"),

        "common.close": ("关闭", "Close"),
        "common.cancel": ("取消", "Cancel"),
        "common.delete": ("删除", "Delete"),
        "common.edit": ("编辑", "Edit"),
        "common.save": ("保存", "Save"),
        "common.clear": ("清空", "Clear"),
        "common.defaultOff": ("默认关闭", "Off by default"),
        "common.off": ("关闭", "Off"),
        "common.on": ("开启", "On"),
        "common.yes": ("是", "Yes"),
        "common.no": ("否", "No"),
        "common.none": ("无", "None"),
        "common.connectVpn": ("连接 VPN", "Connect VPN"),
        "common.disconnectVpn": ("断开 VPN", "Disconnect VPN"),
        "common.enable": ("启用", "Enable"),
        "common.copy": ("复制", "Copy"),

        "vpn.connected": ("已连接", "Connected"),
        "vpn.connecting": ("连接中", "Connecting"),
        "vpn.disconnecting": ("断开中", "Disconnecting"),
        "vpn.reasserting": ("重连中", "Reconnecting"),
        "vpn.invalid": ("无配置", "No Configuration"),
        "vpn.disconnected": ("未连接", "Disconnected"),
        "vpn.unknown": ("未知", "Unknown"),
        "vpn.error.teardownTimeout": ("VPN 扩展尚未完全停止，请稍后重试", "The VPN extension has not fully stopped. Try again shortly."),
        "vpn.error.staleSessionRecovered": ("上次 VPN 会话异常结束，已重置状态。", "The previous VPN session ended unexpectedly and has been reset."),
        "vpn.error.saveConfig": ("保存 VPN 配置失败", "Failed to save VPN configuration"),
        "vpn.error.reloadConfig": ("重新读取 VPN 配置失败", "Failed to reload VPN configuration"),
        "vpn.error.startTunnel": ("启动 VPN 隧道失败", "Failed to start VPN tunnel"),
        "vpn.error.syncConfig": ("同步 VPN 配置失败", "Failed to sync VPN configuration"),
        "vpn.error.reloadSyncedConfig": ("重新读取同步后的 VPN 配置失败", "Failed to reload synced VPN configuration"),
        "vpn.error.packetTunnelStartFailed": ("VPN 扩展启动失败，已回到 OFF。请在 Xcode Console 过滤 OpenPPP2 查看 PacketTunnel 的具体错误。", "VPN extension startup failed and returned to OFF. Filter OpenPPP2 in Xcode Console for PacketTunnel details."),
        "vpn.error.operationFailed": ("VPN 操作失败", "VPN operation failed"),
        "vpn.error.missingServer": ("当前配置缺少有效的 ppp:// server，请先选择或导入节点。", "The current profile is missing a valid ppp:// server. Select or import a profile first."),

        "home.notConnected": ("未连接", "Not Connected"),
        "home.connected": ("已连接", "Connected"),
        "home.connecting": ("连接中...", "Connecting..."),
        "home.disconnecting": ("断开中...", "Disconnecting..."),
        "home.reconnecting": ("重连中...", "Reconnecting..."),
        "home.ready": ("准备连接", "Ready to connect"),
        "home.checkingStatus": ("正在确认连接状态", "Checking connection status"),
        "home.firstProfile": ("导入订阅开始使用，也可在配置文件中手动添加。", "Import a subscription to get started, or add a profile manually below."),
        "home.firstPermission": ("首次连接时，iOS 会请求允许添加 VPN 配置。", "On first connection, iOS asks for permission to add a VPN configuration."),
        "home.profileScope": ("已保存的配置：%@\n已保存的范围：%@", "Saved profile: %@\nSaved scope: %@"),
        "home.tunnelScope": ("VPN 隧道 · %@ · %@", "VPN tunnel · %@ · %@"),
        "home.proxyScope": ("仅代理", "Proxy only"),
        "home.livePath": ("运行状态：%@路径：%@", "Live: %@Path: %@"),
        "home.statusMismatch": ("iOS 与隧道报告的状态不一致。", "iOS and the tunnel report different states."),
        "home.checkAgain": ("重新检查状态", "Check status again"),
        "vpn.error.existingSession": ("请先停止现有 VPN 会话。", "Stop the existing VPN session first."),
        "home.showDetails": ("显示连接详情", "Show connection details"),
        "home.hideDetails": ("收起连接详情", "Hide connection details"),
        "home.vpnStarting": ("VPN 正在启动", "VPN is starting"),
        "home.stopping": ("正在停止 VPN", "Stopping VPN"),
        "home.stopTakingTooLong": ("停止耗时过长", "Stopping is taking longer than expected"),
        "home.networkChanged": ("网络已变化", "Network changed"),
        "home.connect": ("连接", "Connect"),
        "home.stop": ("停止", "Stop"),
        "home.retry": ("重试", "Retry"),
        "home.forceStop": ("强制停止", "Force Stop"),
        "home.noProfile": ("没有配置", "No Profile"),
        "home.selectProfile": ("点击选择一个配置", "Tap to choose a profile"),
        "home.route.geo": ("GEO 分流", "GEO"),
        "home.route.global": ("全局模式", "Global"),
        "home.route.basic": ("基础规则", "Basic"),
        "home.upload": ("↑ 上行", "↑ Upload"),
        "home.download": ("↓ 下行", "↓ Download"),
        "home.quickSwitches": ("快捷开关", "Quick Toggles"),
        "home.lanProxy": ("局域网代理", "LAN Proxy"),
        "home.lanProxy.subtitle": ("HTTP / SOCKS 监听 0.0.0.0", "HTTP / SOCKS listens on 0.0.0.0"),
        "home.blockQuic": ("屏蔽 QUIC", "Block QUIC"),
        "home.blockQuicUpdated": ("QUIC 开关已更新", "QUIC setting updated"),
        "home.routeMode": ("路由模式", "Routing Mode"),
        "home.lanUpdated": ("局域网代理已更新", "LAN proxy updated"),
        "home.routeUpdated": ("路由模式已更新", "Routing mode updated"),
        "home.quickSaved": ("主页快捷设置已保存。当前 VPN 正在运行，要让新参数生效需要重连。", "Home quick settings saved. The running VPN must reconnect before the new options take effect."),
        "home.connectFailed": ("连接失败", "Connection Failed"),
        "home.timeout": ("连接超时（超过 %ds 上限）：请检查服务器地址、密钥与网络连通性。", "Connection timed out after %ds. Check server address, keys, and network reachability."),
        "home.heartbeatTimeout": ("连接超时（扩展心跳已停 %.1fs）：请检查服务器地址、密钥与网络连通性。", "Connection timed out: extension heartbeat stopped for %.1fs. Check server address, keys, and network reachability."),
        "home.state.initializingClient": ("初始化客户端...", "Initializing client..."),
        "home.state.initializingExchanger": ("初始化交换器...", "Initializing exchanger..."),
        "home.state.handshaking": ("握手中...", "Handshaking..."),
        "home.state.startingEngine": ("启动引擎...", "Starting engine..."),

        "settings.debug": ("调试", "Debug"),
        "settings.refreshVPN": ("刷新 VPN 状态", "Refresh VPN Status"),
        "settings.crashReports": ("崩溃收集", "Crash Reports"),
        "settings.telemetry": ("遥测上传", "Telemetry Upload"),
        "settings.systemSettings": ("打开系统设置", "Open System Settings"),
        "settings.resetProfiles": ("清空配置文件", "Reset Profiles"),
        "settings.section.debug": ("调试", "Debug"),
        "settings.section.diagnostics": ("诊断", "Diagnostics"),
        "settings.section.app": ("应用", "App"),
        "settings.section.system": ("系统", "System"),
        "settings.section.danger": ("危险操作", "Danger Zone"),
        "settings.debugEnabled": ("已开启部分调试项", "Some debug options enabled"),
        "settings.section.connection": ("连接与排查", "Connection & troubleshooting"),
        "settings.section.privacy": ("隐私与报告", "Privacy & reports"),
        "settings.section.storage": ("本机数据", "On this device"),
        "settings.systemSettings.detail": ("iOS VPN / App 设置", "iOS VPN / App Settings"),
        "settings.resetProfiles.detail": ("恢复默认配置", "Restore default profiles"),
        "settings.reset.title": ("清空配置文件", "Reset Profiles"),
        "settings.reset.message": ("这会删除所有本地配置、重置启动参数，并恢复默认空白配置。", "This deletes all local profiles, resets launch options, and restores the default blank profile."),
        "settings.reset.action": ("清空", "Reset"),
        "crash.none": ("没有待上传报告", "No pending reports"),
        "crash.none.detail": ("此处没有需要上传或清理的报告。", "There is nothing to send or clear for this process."),
        "crash.partial": ("%@；VPN 扩展报告尚未读取", "%@; VPN extension reports not read"),
        "crash.manage": ("处理报告", "Manage reports"),
        "crash.checking": ("正在读取 VPN 扩展报告…", "Reading VPN extension reports…"),
        "crash.extensionUnavailable": ("无法读取 VPN 扩展报告", "VPN extension reports unavailable"),
        "crash.extensionRecovery": ("请在 VPN 连接后点刷新。", "Refresh while the VPN is connected to check again."),
        "crash.reportID": ("报告 %@", "Report %@"),
        "crash.confirmUpload": ("要将 App 和 VPN 扩展的待处理崩溃报告及遥测 ID 发送至 %@ 吗？成功上传的报告会从本机删除。", "Send pending app and VPN extension crash reports with the telemetry ID to %@? Successfully uploaded reports are deleted from this device."),
        "crash.savesSettings": ("此操作也会保存本页的遥测设置。", "This also saves the telemetry settings on this page."),
        "crash.generating": ("上次启动崩溃，报告正在生成", "Last launch crashed; report is being generated"),
        "crash.pending": ("%d 个待上传报告", "%d pending reports"),
        "crash.pendingWithProcess": ("%d 个待上传报告（%@）", "%d pending reports (%@)"),
        "crash.appGroup": ("App Group 共享目录", "App Group shared container"),
        "crash.localCache": ("App 本地缓存；VPN 扩展需在已连接时读取", "App local cache; VPN extension reports require an active connection"),
        "crash.status": ("状态", "Status"),
        "crash.storage": ("存储位置", "Storage"),
        "crash.uploadToOtel": ("上传到 OpenTelemetry", "Upload to OpenTelemetry"),
        "crash.clearLocal": ("清空本地报告", "Clear Local Reports"),
        "crash.pendingHeader": ("%@ 待上传报告", "%@ pending reports"),
        "crash.uploading": ("上传中...", "Uploading..."),
        "crash.uploadDeletesLocal": ("成功上传后会删除本地报告", "Local reports are deleted after successful upload"),
        "crash.clearLocal.detail": ("删除尚未上传的崩溃报告", "Delete crash reports that have not been uploaded"),
        "crash.vpnDisconnected": ("VPN 未连接", "VPN Disconnected"),
        "crash.vpnDisconnected.detail": ("连接后可读取扩展沙盒中的报告", "Connect VPN to read reports from the extension sandbox"),
        "crash.waitingUpload": ("等待 OpenTelemetry 上传", "Waiting for OpenTelemetry upload"),
        "crash.clear.title": ("清空崩溃报告", "Clear Crash Reports"),
        "crash.clear.message": ("这会删除本机尚未上传的 KSCrash 报告。", "This deletes local KSCrash reports that have not been uploaded."),
        "crash.uploadDisabled.title": ("遥测上传未开启", "Telemetry Upload Disabled"),
        "crash.uploadDisabled.message": ("请先在设置里的「遥测上传」开启上传并配置 endpoint。", "Enable telemetry upload and configure an endpoint in Settings first."),
        "crash.uploadFinished": ("上传完成", "Upload Finished"),
        "crash.vpnReportUnavailable": ("无法读取扩展沙盒中的报告", "Extension sandbox reports could not be read"),
        "crash.process.app": ("App", "App"),
        "crash.process.packetTunnel": ("VPN 扩展", "VPN Extension"),

        "debug.title": ("调试", "Debug"),
        "debug.section.ui": ("界面", "Interface"),
        "debug.section.local": ("本地数据包诊断", "Local packet diagnostics"),
        "debug.item.panel": ("调试面板", "Debug Panel"),
        "debug.item.packetDiagnostics": ("包级诊断", "Packet Diagnostics"),
        "debug.item.consoleLogging": ("包级 Console 日志", "Packet Console Logging"),
        "debug.item.packetTelemetry": ("上传 packet_flow", "Upload packet_flow"),
        "debug.item.panel.detail": ("显示 App 内调试入口", "Show in-app debug entry points"),
        "debug.item.packetDiagnostics.detail": ("记录包摘要和诊断快照；关闭时热路径只做轻量计数", "Record packet summaries and diagnostic snapshots; hot paths only keep light counters when off"),
        "debug.item.consoleLogging.detail": ("把包流摘要写入 Xcode Console；Release 默认关闭", "Write packet-flow summaries to Xcode Console; off by default in Release"),
        "debug.item.packetTelemetry.detail": ("把采样 packet_flow 上传到当前 OTLP endpoint", "Upload sampled packet_flow events to the current OTLP endpoint"),
        "debug.packetTunnel.footer": ("Packet Tunnel 调试项会写入 VPN 配置；修改后请断开并重连 VPN。默认全部关闭，以免影响 speedtest / YouTube 这类高吞吐路径。", "Packet Tunnel debug options are written into the VPN configuration. Disconnect and reconnect VPN after changing them. They default to off to avoid affecting high-throughput paths such as speedtest and YouTube."),

        "telemetry.destination.developer": ("开发者默认", "Developer Default"),
        "telemetry.destination.custom": ("自定义", "Custom"),
        "telemetry.identity": ("身份", "Identity"),
        "telemetry.upload": ("上传", "Upload"),
        "telemetry.data": ("数据", "Data"),
        "telemetry.enableUpload": ("启用上传", "Enable Upload"),
        "telemetry.enableUpload.detail": ("保存后，所选数据可发送到下方地址。", "Once saved, the selected data can be sent to the destination below."),
        "telemetry.endpoint": ("OTLP HTTP 接收地址", "OTLP HTTP endpoint"),
        "telemetry.destination": ("上传目标", "Upload destination"),
        "telemetry.native": ("原生日志与跟踪", "Native logs and traces"),
        "telemetry.metrics": ("指标", "Metrics"),
        "telemetry.metrics.detail": ("计数器、仪表和直方图", "Counters, gauges and histograms"),
        "telemetry.spans": ("跟踪区段", "Spans"),
        "telemetry.spans.detail": ("包含跟踪区段事件", "Include trace span events"),
        "telemetry.device": ("设备：%@ / %@ %@", "Device: %@ / %@ %@"),
        "telemetry.logLevel": ("日志详细程度", "Log detail"),
        "telemetry.info": ("信息", "Info"),
        "telemetry.verbose": ("详细", "Verbose"),
        "telemetry.debug": ("调试", "Debug"),
        "telemetry.trace": ("跟踪", "Trace"),
        "telemetry.copyPrivacy": ("此 ID 用于关联本设备的遥测数据。要将其复制到本机剪贴板吗？", "This ID links telemetry from this device. Copy it to the local clipboard?"),
        "telemetry.saveConsent": ("保存上传设置？", "Save upload settings?"),
        "telemetry.saveConsent.detail": ("所选诊断数据和遥测 ID 将允许发送至 %@。", "The selected diagnostic data and telemetry ID may be sent to %@."),
        "telemetry.uploadSetup": ("如需发送报告，请启用上传、配置接收地址，并选中崩溃报告。", "To send reports, enable upload, configure a destination and select crash reports."),
        "telemetry.crash.detail": ("App 与 VPN 扩展崩溃报告", "App and VPN extension crash reports"),
        "telemetry.native.detail": ("OpenPPP2 日志、计数与 Span", "OpenPPP2 logs, counters, and spans"),
        "telemetry.extensionNote": ("Trace 日志和 Spans 仅在上传与 Native Telemetry 开启后生效；Metrics 在 VPN 扩展内保持关闭。", "Trace logs and spans take effect only when upload and Native Telemetry are enabled; metrics remain disabled in the VPN extension."),
        "telemetry.copyMachineId": ("复制遥测 ID", "Copy Telemetry ID"),
        "telemetry.shortId": ("短 ID: %@", "Short ID: %@"),
        "telemetry.developerEndpointMissing": ("开发者默认 endpoint 未配置", "Developer default endpoint is not configured"),
        "telemetry.uploadCrashReports": ("上传崩溃报告", "Upload Crash Reports"),
        "telemetry.uploading": ("上传中...", "Uploading..."),
        "telemetry.machineCopied": ("遥测 ID 已复制", "Telemetry ID copied"),
        "telemetry.endpointEmpty.title": ("Endpoint 为空", "Endpoint Empty"),
        "telemetry.endpointEmpty.message": ("请填写 OTLP HTTP endpoint。", "Enter an OTLP HTTP endpoint."),
        "telemetry.endpointInvalid.title": ("Endpoint 无效", "Invalid Endpoint"),
        "telemetry.endpointInvalid.message": ("请填写 http:// 或 https:// 开头的 OTLP HTTP endpoint。", "Enter an OTLP HTTP endpoint starting with http:// or https://."),
        "telemetry.saved": ("遥测设置已保存", "Telemetry settings saved"),
        "telemetry.restartMessage": ("遥测设置已保存。当前 VPN 正在运行，要让 Native Telemetry 生效需要重连。", "Telemetry settings saved. The running VPN must reconnect before Native Telemetry takes effect."),
        "telemetry.cannotUpload.title": ("不能上传", "Cannot Upload"),
        "telemetry.cannotUpload.message": ("请先开启上传并配置 endpoint。", "Enable upload and configure an endpoint first."),
        "telemetry.vpnUploadUnavailable": ("VPN 扩展未能上传其报告", "VPN extension report upload is unavailable"),
        "telemetry.summarySkipped": ("已跳过上传", "Upload skipped"),
        "telemetry.summaryNoReports": ("没有待上传报告", "No pending reports"),
        "telemetry.summaryUploaded": ("已上传 %d 个报告", "Uploaded %d reports"),
        "telemetry.summaryUploadedFailed": ("已上传 %d 个，失败 %d 个", "Uploaded %d, failed %d"),

        "profiles.title": ("配置文件", "Profiles"),
        "profiles.selectTitle": ("选择节点", "Select Profile"),
        "profiles.search": ("搜索配置名称或地址...", "Search profile name or address..."),
        "profiles.actions": ("配置操作", "Profile actions"),
        "profiles.noMatches": ("没有匹配的配置", "No matching profiles"),
        "profiles.clearSearch": ("清除搜索，显示全部配置", "Clear search and show all profiles"),
        "profiles.savedSelection": ("已选配置 · %@", "Selected profile · %@"),
        "profiles.favorites": ("收藏", "Favorites"),
        "profiles.favorite": ("收藏", "Favorite"),
        "profiles.unfavorite": ("取消收藏", "Unfavorite"),
        "profiles.locations": ("节点", "Locations"),
        "profiles.importSubscription": ("导入远程订阅", "Import Remote Subscription"),
        "profiles.importFile": ("从文件导入", "Import from File"),
        "profiles.exportAll": ("导出全部配置", "Export All Profiles"),
        "profiles.importFailed": ("导入失败", "Import Failed"),
        "profiles.exportFailed": ("导出失败", "Export Failed"),
        "profiles.exported": ("配置已导出", "Profiles exported"),
        "profiles.import": ("导入", "Import"),
        "profiles.invalidSubscription": ("订阅地址无效", "Invalid Subscription URL"),
        "profiles.invalidSubscription.message": ("订阅地址必须是 HTTPS URL；仅允许本机开发 HTTP 地址。", "Subscription URL must use HTTPS; only local development HTTP URLs are allowed."),
        "profiles.fetching": ("正在拉取订阅...", "Fetching subscription..."),
        "profiles.subscriptionFailed": ("订阅导入失败", "Subscription Import Failed"),
        "profiles.subscriptionBadResponse": ("响应为空、过大或不是 UTF-8。", "Response is empty, too large, or not UTF-8."),
        "profiles.importedNodes": ("已导入/更新 %d 个节点", "Imported/updated %d nodes"),
        "profiles.apply": ("应用", "Use"),
        "profiles.share": ("分享", "Share"),
        "profiles.pin": ("置顶", "Pin"),
        "profiles.unpin": ("取消置顶", "Unpin"),
        "profiles.group.pinned": ("已置顶", "Pinned"),
        "profiles.group.local": ("本地配置", "Local Profiles"),
        "profiles.group.subscription": ("订阅 · %@", "Subscription · %@"),
        "profiles.addMenu.new": ("新增配置", "New Profile"),
        "profiles.deleteProfile": ("删除配置", "Delete Profile"),
        "profiles.deleteProfile.message": ("确定要删除「%@」吗？", "Delete “%@”?"),
        "profiles.switched.message": ("已切换到「%@」。当前 VPN 正在运行，要让新配置生效需要重连。", "Switched to “%@”. The running VPN must reconnect before the new profile takes effect."),
        "profiles.active": ("使用中", "ACTIVE"),
        "profiles.noServer": ("未配置服务器", "No server configured"),
        "profiles.lanOn": ("LAN 开启", "LAN On"),
        "profiles.lanOff": ("LAN 关闭", "LAN Off"),

        "options.title": ("启动参数", "Launch Options"),
        "options.save": ("保存", "Save"),
        "options.scope": ("这些启动参数适用于所有配置。保存后将在下次连接时使用。", "These options apply to all profiles. Save to use them on the next connection."),
        "options.connection": ("连接", "Connection"),
        "options.tunAddress": ("TUN 地址", "TUN address"),
        "options.tunMask": ("TUN 子网掩码", "TUN subnet mask"),
        "options.tunPrefix": ("TUN 前缀长度", "TUN prefix length"),
        "options.gateway": ("网关", "Gateway"),
        "options.bypass": ("直连 IP / CIDR", "Bypass IP / CIDR"),
        "options.dnsRules": ("DNS 规则", "DNS rules"),
        "options.domestic": ("境内 DNS", "Domestic DNS"),
        "options.foreign": ("境外 DNS", "Foreign DNS"),
        "options.fakeRange": ("Fake-IP 地址范围", "Fake-IP range"),
        "options.stun": ("STUN 候选地址", "STUN candidates"),
        "options.country": ("Geo 国家代码", "Geo country code"),
        "options.fakeIp.detail": ("即时生成虚拟 A 记录", "Generate synthetic A records immediately"),
        "options.ecs.detail": ("EDNS 客户端子网", "EDNS client subnet"),
        "options.staticMode": ("静态模式", "Static mode"),
        "options.portError": ("HTTP 和 SOCKS 端口须为 1 到 65535 的整数。", "Enter an integer from 1 to 65535 for each proxy port."),
        "options.advancedError": ("在高级参数中检查：TUN 前缀为 0–32，MTU 为 1–65535，VNet Mux 为非负整数。", "Check Advanced Options: TUN prefix must be 0–32, MTU 1–65535, and VNet Mux a nonnegative integer."),
        "options.compat": ("兼容", "Compat"),
        "options.muxMode": ("复用模式", "Mux mode"),
        "options.effective": ("隧道上报的模式：%@", "Tunnel-reported mode: %@"),
        "options.effectiveUnknown": ("暂无隧道上报的复用模式", "No tunnel-reported mux mode available"),
        "options.advancedSave": ("返回「启动参数」并点保存。新设置将在下次连接时生效。", "Return to Launch Options and tap Save. New settings take effect on the next connection."),
        "options.resetDefaults": ("恢复默认", "Reset"),
        "options.reset.title": ("恢复默认启动参数", "Reset Launch Options"),
        "options.reset.message": ("这会把当前页面字段恢复为默认值；点击保存后才会写入。", "This restores the fields on this page to defaults. The values are written only after you tap Save."),
        "options.reset.action": ("恢复默认", "Reset"),
        "options.proxy": ("代理", "Proxy"),
        "options.httpProxyPort": ("HTTP 端口", "HTTP Port"),
        "options.socksProxyPort": ("SOCKS 端口", "SOCKS Port"),
        "options.allowLan": ("允许局域网代理", "Allow LAN Proxy"),
        "options.allowLan.detail": ("HTTP / SOCKS 监听 0.0.0.0", "HTTP / SOCKS listens on 0.0.0.0"),
        "options.dns": ("DNS", "DNS"),
        "options.geoBypass": ("区域与直连规则", "Geo & bypass rules"),
        "options.tlsVerify": ("TLS 校验", "TLS Verification"),
        "options.tlsVerify.detail": ("验证 DoH / DoT 上游证书", "Verify DoH / DoT upstream certificates"),
        "options.geoEnabled": ("启用 Geo 规则", "Enable Geo Rules"),
        "options.geoEnabled.detail": ("生成 bypass 与 dns-rules", "Generate bypass and dns-rules"),
        "options.routeMode.detail": ("GEO 使用规则分流；全局忽略 bypass；基础保留内网 bypass", "GEO uses rule routing; Global ignores bypass; Basic keeps private-network bypass"),
        "options.tun": ("TUN 接口", "TUN Interface"),
        "options.advanced": ("高级", "Advanced"),
        "options.advancedParams": ("高级参数", "Advanced Options"),
        "options.advanced.detail": ("区域分流、DNS 规则、TUN 接口与传输模式", "Geo routing, DNS rules, TUN interface and transport modes"),
        "options.vnet.detail": ("启用虚拟网卡路径", "Enable virtual network path"),
        "options.network": ("网络", "Network"),
        "options.blockQuic.detail": ("屏蔽 UDP/443 防止绕过", "Block UDP/443 to prevent bypass"),
        "options.staticMode.detail": ("UDP 静态隧道模式", "UDP static tunnel mode"),
        "options.autoReconnect": ("网络恢复后自动重连", "Auto Reconnect after Network Recovery"),
        "options.autoReconnect.detail": ("断网恢复时自动重连当前 VPN", "Reconnect the current VPN after network recovery"),
        "options.noProfile.title": ("没有配置文件", "No Profile"),
        "options.noProfile.message": ("请先在配置文件页创建并选择一个配置。", "Create and select a profile first."),
        "options.saved.message": ("启动参数已保存。当前 VPN 正在运行，要让新参数生效需要重连。", "Launch options saved. The running VPN must reconnect before the new options take effect."),
        "options.saved.toast": ("启动参数已保存", "Launch options saved"),

        "editor.addTitle": ("新增配置", "New Profile"),
        "editor.editTitle": ("编辑配置", "Edit Profile"),
        "editor.name": ("名称", "Name"),
        "editor.subtitle": ("副标题 / 城市", "Subtitle / City"),
        "editor.flag": ("图标 / Emoji", "Icon / Emoji"),
        "editor.basic": ("基本信息", "Basic Info"),
        "editor.server": ("服务器", "Server"),
        "editor.crypto": ("加密", "Crypto"),
        "editor.host": ("主机地址", "Host"),
        "editor.port": ("端口（可选）", "Port (optional)"),
        "editor.bandwidth": ("带宽", "Bandwidth"),
        "editor.protocol": ("协议加密算法", "Protocol cipher"),
        "editor.protocolKey": ("协议密钥", "Protocol key"),
        "editor.transport": ("传输加密算法", "Transport cipher"),
        "editor.transportKey": ("传输密钥", "Transport key"),
        "editor.showAdvanced": ("显示带宽与原始 JSON", "Show bandwidth and raw JSON"),
        "editor.hideAdvanced": ("收起带宽与原始 JSON", "Hide bandwidth and raw JSON"),
        "editor.portError": ("请输入 1 到 65535 的端口，或留空。", "Enter a port from 1 to 65535, or leave it blank."),
        "editor.numberError": ("请输入非负整数，或留空。", "Enter a nonnegative integer, or leave this blank."),
        "editor.nameError": ("请输入配置名称以便识别此配置。", "Enter a name so you can identify this profile."),
        "editor.localProxy": ("本地代理", "Local Proxy"),
        "editor.rawJson": ("高级 Raw JSON", "Advanced Raw JSON"),
        "editor.saveProfile": ("保存配置", "Save Profile"),
        "editor.export": ("导出配置", "Export Profile"),
        "editor.exportUnavailable": ("无法导出", "Cannot Export"),
        "editor.saveBeforeExport": ("请先保存配置后再导出。", "Save the profile before exporting."),
        "editor.exported": ("配置已导出", "Profile exported"),
        "editor.exportFailed": ("导出失败", "Export Failed"),
        "editor.rawJsonError": ("Raw JSON 格式错误", "Raw JSON Error"),
        "editor.rawJsonError.message": ("请检查高级选项中的原始 JSON。顶层必须是 JSON 对象；当前修改尚未保存。", "Check the raw JSON under advanced options. It must be a JSON object. Your changes have not been saved."),
        "editor.restartAfterSave": ("当前 VPN 正在运行，保存后的配置需要重连才会生效。", "The running VPN must reconnect before the saved profile takes effect."),
        "importExport.secretsWarning": ("导出的文件包含协议密钥等敏感信息，请妥善保管，不要分享给不可信的人。", "Exported files contain protocol keys and other secrets. Keep them safe and do not share them with untrusted people."),
        "importExport.continue": ("继续", "Continue"),
        "importExport.importTitle": ("导入配置文件", "Import Profiles"),
        "importExport.importMessage": ("文件包含 %d 个配置。请选择导入方式。", "The file contains %d profiles. Choose how to import them."),
        "importExport.merge": ("合并到本地", "Merge into Local"),
        "importExport.replace": ("替换全部", "Replace All"),
        "importExport.confirmMerge": ("确认合并导入", "Confirm Merge Import"),
        "importExport.confirmReplace": ("确认替换全部配置", "Confirm Replace All"),
        "importExport.noneImported": ("未导入任何配置", "No profiles imported"),
        "importExport.addedUpdated": ("已新增 %d 个，更新 %d 个", "Added %d, updated %d"),
        "importExport.imported": ("已导入 %d 个配置", "Imported %d profiles"),

        "restart.title": ("重连以应用配置", "Reconnect to Apply"),
        "restart.later": ("稍后", "Later"),
        "restart.now": ("立即重连", "Reconnect Now"),
        "restart.success": ("已重连并应用配置", "Reconnected and applied"),
        "restart.failed": ("重连失败", "Reconnect Failed")
    ]
}

// MARK: - UI Helpers

enum AppTheme {
    static let horizontalPadding: CGFloat = 18
    static let primary = UIColor(red: 0.0, green: 122.0 / 255.0, blue: 1.0, alpha: 1.0)
    static let primarySoft = UIColor(red: 0.0, green: 122.0 / 255.0, blue: 1.0, alpha: 0.12)
    static let primaryMuted = UIColor(red: 0.0, green: 122.0 / 255.0, blue: 1.0, alpha: 0.18)
}

final class PowerButton: UIControl {
    private let outer = UIView()
    private let middle = UIView()
    private let inner = UIView()
    private let symbol = UIImageView(image: UIImage(systemName: "power"))
    private let spinner = UIActivityIndicatorView(style: .large)
    private let statusLabel = UILabel()
    private let detailLabel = UILabel()
    private let offColor = UIColor { trait in
        trait.userInterfaceStyle == .dark
            ? UIColor(red: 0.16, green: 0.24, blue: 0.34, alpha: 1)
            : UIColor(red: 0.18, green: 0.25, blue: 0.35, alpha: 1)
    }

    override init(frame: CGRect) {
        super.init(frame: frame)
        setup()
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    private func setup() {
        isAccessibilityElement = true
        accessibilityTraits = [.button]

        [outer, middle, inner, symbol, spinner, statusLabel, detailLabel].forEach { item in
            item.translatesAutoresizingMaskIntoConstraints = false
            item.isUserInteractionEnabled = false
        }
        addSubview(outer)
        addSubview(middle)
        addSubview(inner)
        addSubview(symbol)
        addSubview(spinner)
        addSubview(statusLabel)
        addSubview(detailLabel)
        symbol.tintColor = .white
        symbol.contentMode = .scaleAspectFit
        spinner.color = .white
        statusLabel.font = .systemFont(ofSize: 19, weight: .semibold)
        statusLabel.textAlignment = .center
        statusLabel.textColor = .label
        statusLabel.numberOfLines = 1
        detailLabel.font = .monospacedDigitSystemFont(ofSize: 13, weight: .medium)
        detailLabel.textAlignment = .center
        detailLabel.textColor = .secondaryLabel
        detailLabel.numberOfLines = 1

        NSLayoutConstraint.activate([
            outer.centerXAnchor.constraint(equalTo: centerXAnchor),
            outer.topAnchor.constraint(equalTo: topAnchor, constant: 4),
            outer.widthAnchor.constraint(equalToConstant: 184),
            outer.heightAnchor.constraint(equalTo: outer.widthAnchor),
            middle.centerXAnchor.constraint(equalTo: outer.centerXAnchor),
            middle.centerYAnchor.constraint(equalTo: outer.centerYAnchor),
            middle.widthAnchor.constraint(equalToConstant: 142),
            middle.heightAnchor.constraint(equalTo: middle.widthAnchor),
            inner.centerXAnchor.constraint(equalTo: outer.centerXAnchor),
            inner.centerYAnchor.constraint(equalTo: outer.centerYAnchor),
            inner.widthAnchor.constraint(equalToConstant: 104),
            inner.heightAnchor.constraint(equalTo: inner.widthAnchor),
            symbol.centerXAnchor.constraint(equalTo: inner.centerXAnchor),
            symbol.centerYAnchor.constraint(equalTo: inner.centerYAnchor),
            symbol.widthAnchor.constraint(equalToConstant: 46),
            symbol.heightAnchor.constraint(equalToConstant: 46),
            spinner.centerXAnchor.constraint(equalTo: inner.centerXAnchor),
            spinner.centerYAnchor.constraint(equalTo: inner.centerYAnchor),
            statusLabel.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 16),
            statusLabel.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -16),
            statusLabel.topAnchor.constraint(equalTo: outer.bottomAnchor, constant: 10),
            detailLabel.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 16),
            detailLabel.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -16),
            detailLabel.topAnchor.constraint(equalTo: statusLabel.bottomAnchor, constant: 4),
            detailLabel.bottomAnchor.constraint(lessThanOrEqualTo: bottomAnchor, constant: -2)
        ])
        apply(isOn: false, isBusy: false, status: "Not Connected", detail: "")
    }

    override func layoutSubviews() {
        super.layoutSubviews()
        [outer, middle, inner].forEach { $0.layer.cornerRadius = $0.bounds.width / 2 }
        inner.layer.shadowRadius = 28
        inner.layer.shadowOpacity = 0.34
        inner.layer.shadowOffset = CGSize(width: 0, height: 10)
    }

    func apply(isOn: Bool, isBusy: Bool, status: String, detail: String) {
        let color: UIColor = isBusy || isOn ? AppTheme.primary : offColor
        outer.backgroundColor = color.withAlphaComponent(0.06)
        middle.backgroundColor = color.withAlphaComponent(0.12)
        inner.backgroundColor = color
        inner.layer.shadowColor = color.cgColor
        symbol.isHidden = isBusy
        isBusy ? spinner.startAnimating() : spinner.stopAnimating()
        statusLabel.text = status
        detailLabel.text = detail
        detailLabel.isHidden = detail.isEmpty
        updateBreathing(isOn: isOn, isBusy: isBusy)
        accessibilityLabel = isOn ? L10n.tr("common.disconnectVpn") : L10n.tr("common.connectVpn")
        accessibilityValue = isBusy ? status : (isOn ? "\(L10n.tr("vpn.connected")) \(detail)" : L10n.tr("vpn.disconnected"))
    }

    private func updateBreathing(isOn: Bool, isBusy: Bool) {
        let key = "openppp2.breathing"
        guard !isOn && !isBusy else {
            outer.layer.removeAnimation(forKey: key)
            middle.layer.removeAnimation(forKey: key)
            return
        }

        if outer.layer.animation(forKey: key) == nil {
            let animation = CABasicAnimation(keyPath: "opacity")
            animation.fromValue = 0.58
            animation.toValue = 1.0
            animation.duration = 1.8
            animation.autoreverses = true
            animation.repeatCount = .infinity
            animation.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
            outer.layer.add(animation, forKey: key)
        }
        if middle.layer.animation(forKey: key) == nil {
            let animation = CABasicAnimation(keyPath: "transform.scale")
            animation.fromValue = 0.98
            animation.toValue = 1.035
            animation.duration = 1.8
            animation.autoreverses = true
            animation.repeatCount = .infinity
            animation.timingFunction = CAMediaTimingFunction(name: .easeInEaseOut)
            middle.layer.add(animation, forKey: key)
        }
    }
}

final class StatCard: UIView {
    private let valueLabel = UILabel()
    private let subtitleLabel = UILabel()

    init(title: String, symbol: String, tint: UIColor) {
        super.init(frame: .zero)
        backgroundColor = .secondarySystemGroupedBackground
        layer.cornerRadius = 12
        translatesAutoresizingMaskIntoConstraints = false

        let icon = UIImageView(image: UIImage(systemName: symbol))
        icon.tintColor = tint
        icon.contentMode = .scaleAspectFit
        icon.translatesAutoresizingMaskIntoConstraints = false
        icon.widthAnchor.constraint(equalToConstant: 22).isActive = true
        icon.heightAnchor.constraint(equalToConstant: 22).isActive = true

        let titleLabel = UILabel()
        titleLabel.text = title
        titleLabel.font = .preferredFont(forTextStyle: .footnote)
        titleLabel.textColor = .secondaryLabel
        titleLabel.textAlignment = .center

        valueLabel.font = .monospacedDigitSystemFont(ofSize: 17, weight: .bold)
        valueLabel.textAlignment = .center
        valueLabel.adjustsFontSizeToFitWidth = true
        valueLabel.minimumScaleFactor = 0.72
        valueLabel.numberOfLines = 1
        subtitleLabel.font = .preferredFont(forTextStyle: .caption1)
        subtitleLabel.textColor = .secondaryLabel
        subtitleLabel.textAlignment = .center
        subtitleLabel.adjustsFontSizeToFitWidth = true
        subtitleLabel.minimumScaleFactor = 0.76
        subtitleLabel.numberOfLines = 1
        let stack = UIStackView(arrangedSubviews: [icon, titleLabel, valueLabel, subtitleLabel])
        stack.axis = .vertical
        stack.alignment = .center
        stack.spacing = 6
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 12),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -12),
            stack.topAnchor.constraint(equalTo: topAnchor, constant: 12),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -12),
            heightAnchor.constraint(greaterThanOrEqualToConstant: 124)
        ])
        set(value: "0 B/s", subtitle: "总 0 B")
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    func set(value: String, subtitle: String) {
        valueLabel.text = value
        subtitleLabel.text = subtitle
    }
}

final class FormTextField: UIView {
    private let label = UILabel()
    let field = UITextField()

    var text: String? {
        get { field.text }
        set { field.text = newValue }
    }

    var textValue: String {
        (field.text ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
    }

    init(label title: String, keyboard: UIKeyboardType = .default) {
        super.init(frame: .zero)
        label.text = title
        label.font = .preferredFont(forTextStyle: .caption1)
        label.numberOfLines = 0
        label.adjustsFontForContentSizeCategory = true
        label.isAccessibilityElement = false
        field.accessibilityLabel = title
        field.adjustsFontForContentSizeCategory = true
        label.textColor = .secondaryLabel
        field.borderStyle = .none
        field.keyboardType = keyboard
        field.autocapitalizationType = .none
        field.autocorrectionType = .no
        field.font = .preferredFont(forTextStyle: .body)
        field.textColor = .label
        field.layer.cornerRadius = 10
        field.layer.borderWidth = 1 / UIScreen.main.scale
        field.leftView = UIView(frame: CGRect(x: 0, y: 0, width: 10, height: 1))
        field.leftViewMode = .always
        field.rightView = UIView(frame: CGRect(x: 0, y: 0, width: 10, height: 1))
        field.rightViewMode = .always
        field.heightAnchor.constraint(greaterThanOrEqualToConstant: 42).isActive = true
        applyChrome()
        let stack = UIStackView(arrangedSubviews: [label, field])
        stack.axis = .vertical
        stack.spacing = 6
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor),
            stack.topAnchor.constraint(equalTo: topAnchor),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor)
        ])
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        applyChrome()
    }

    override func traitCollectionDidChange(_ previousTraitCollection: UITraitCollection?) {
        super.traitCollectionDidChange(previousTraitCollection)
        applyChrome()
    }

    private func applyChrome() {
        field.backgroundColor = .tertiarySystemGroupedBackground
        field.layer.borderColor = UIColor.separator.withAlphaComponent(0.65).cgColor
    }
}

final class FormTextView: UIView {
    private let label = UILabel()
    let textView = UITextView()

    var text: String? {
        get { textView.text }
        set { textView.text = newValue }
    }

    var textValue: String {
        (textView.text ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
    }

    init(label title: String) {
        super.init(frame: .zero)
        label.text = title
        label.font = .preferredFont(forTextStyle: .caption1)
        label.numberOfLines = 0
        label.adjustsFontForContentSizeCategory = true
        label.isAccessibilityElement = false
        textView.accessibilityLabel = title
        textView.adjustsFontForContentSizeCategory = true
        label.textColor = .secondaryLabel
        textView.font = UIFontMetrics(forTextStyle: .footnote).scaledFont(for: .monospacedSystemFont(ofSize: 13, weight: .regular))
        textView.textColor = .label
        textView.layer.borderWidth = 1 / UIScreen.main.scale
        textView.layer.cornerRadius = 10
        textView.textContainerInset = UIEdgeInsets(top: 10, left: 8, bottom: 10, right: 8)
        textView.heightAnchor.constraint(equalToConstant: 90).isActive = true
        applyChrome()
        let stack = UIStackView(arrangedSubviews: [label, textView])
        stack.axis = .vertical
        stack.spacing = 6
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor),
            stack.topAnchor.constraint(equalTo: topAnchor),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor)
        ])
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        applyChrome()
    }

    override func traitCollectionDidChange(_ previousTraitCollection: UITraitCollection?) {
        super.traitCollectionDidChange(previousTraitCollection)
        applyChrome()
    }

    private func applyChrome() {
        textView.backgroundColor = .tertiarySystemGroupedBackground
        textView.layer.borderColor = UIColor.separator.withAlphaComponent(0.72).cgColor
    }
}

final class SectionView: UIView {
    init(title: String, symbol: String, views: [UIView]) {
        super.init(frame: .zero)
        backgroundColor = .secondarySystemGroupedBackground
        layer.cornerRadius = 16
        layer.borderWidth = 1 / UIScreen.main.scale
        applyChrome()
        translatesAutoresizingMaskIntoConstraints = false

        let icon = UIImageView(image: UIImage(systemName: symbol))
        icon.tintColor = AppTheme.primary
        icon.contentMode = .scaleAspectFit
        icon.widthAnchor.constraint(equalToConstant: 18).isActive = true
        icon.heightAnchor.constraint(equalToConstant: 18).isActive = true

        let label = UILabel()
        label.text = title
        label.font = .preferredFont(forTextStyle: .headline)
        label.adjustsFontForContentSizeCategory = true
        label.numberOfLines = 0
        label.accessibilityTraits.insert(.header)
        label.textColor = .secondaryLabel
        let header = UIStackView(arrangedSubviews: [icon, label])
        header.axis = .horizontal
        header.spacing = 8
        header.alignment = .center

        let stack = UIStackView(arrangedSubviews: [header] + views)
        stack.axis = .vertical
        stack.spacing = 13
        stack.translatesAutoresizingMaskIntoConstraints = false
        addSubview(stack)
        NSLayoutConstraint.activate([
            stack.leadingAnchor.constraint(equalTo: leadingAnchor, constant: 16),
            stack.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -16),
            stack.topAnchor.constraint(equalTo: topAnchor, constant: 14),
            stack.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -16)
        ])
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func didMoveToWindow() {
        super.didMoveToWindow()
        applyChrome()
    }

    override func traitCollectionDidChange(_ previousTraitCollection: UITraitCollection?) {
        super.traitCollectionDidChange(previousTraitCollection)
        applyChrome()
    }

    private func applyChrome() {
        backgroundColor = .secondarySystemGroupedBackground
        layer.borderColor = UIColor.separator.withAlphaComponent(0.40).cgColor
    }
}

final class PaddingLabel: UILabel {
    var insets = UIEdgeInsets(top: 12, left: 12, bottom: 12, right: 12)

    override func drawText(in rect: CGRect) {
        super.drawText(in: rect.inset(by: insets))
    }

    override var intrinsicContentSize: CGSize {
        let size = super.intrinsicContentSize
        return CGSize(width: size.width + insets.left + insets.right, height: size.height + insets.top + insets.bottom)
    }
}

func row(_ views: [UIView], weights: [CGFloat]) -> UIStackView {
    let stack = UIStackView(arrangedSubviews: views)
    stack.axis = .horizontal
    stack.spacing = 8
    stack.distribution = .fillProportionally
    for (index, view) in views.enumerated() {
        view.setContentHuggingPriority(.defaultLow, for: .horizontal)
        view.widthAnchor.constraint(equalTo: stack.widthAnchor, multiplier: weights[index] / weights.reduce(0, +), constant: -8).priority = .defaultLow
    }
    return stack
}

func switchRow(title: String, subtitle: String, control: UISwitch) -> UIView {
    control.onTintColor = AppTheme.primary
    control.accessibilityLabel = title
    control.accessibilityHint = subtitle
    let titleLabel = UILabel()
    titleLabel.text = title
    titleLabel.font = .preferredFont(forTextStyle: .body)
    titleLabel.numberOfLines = 0
    titleLabel.adjustsFontForContentSizeCategory = true
    titleLabel.isAccessibilityElement = false
    let subtitleLabel = UILabel()
    subtitleLabel.text = subtitle
    subtitleLabel.font = .preferredFont(forTextStyle: .caption1)
    subtitleLabel.textColor = .secondaryLabel
    subtitleLabel.numberOfLines = 0
    subtitleLabel.adjustsFontForContentSizeCategory = true
    subtitleLabel.isAccessibilityElement = false
    let labels = UIStackView(arrangedSubviews: [titleLabel, subtitleLabel])
    labels.axis = .vertical
    labels.spacing = 2
    let row = UIStackView(arrangedSubviews: [labels, control])
    row.axis = .horizontal
    row.spacing = 12
    row.alignment = .center
    labels.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
    return row
}

func disclosureRow(title: String, subtitle: String) -> UIControl {
    let titleLabel = UILabel()
    titleLabel.text = title
    titleLabel.font = .preferredFont(forTextStyle: .body)
    titleLabel.numberOfLines = 0
    titleLabel.adjustsFontForContentSizeCategory = true
    titleLabel.isUserInteractionEnabled = false

    let subtitleLabel = UILabel()
    subtitleLabel.text = subtitle
    subtitleLabel.font = .preferredFont(forTextStyle: .caption1)
    subtitleLabel.textColor = .secondaryLabel
    subtitleLabel.numberOfLines = 0
    subtitleLabel.adjustsFontForContentSizeCategory = true
    subtitleLabel.isUserInteractionEnabled = false

    let labels = UIStackView(arrangedSubviews: [titleLabel, subtitleLabel])
    labels.axis = .vertical
    labels.spacing = 2
    labels.isUserInteractionEnabled = false

    let chevron = UIImageView(image: UIImage(systemName: "chevron.right"))
    chevron.tintColor = .tertiaryLabel
    chevron.contentMode = .scaleAspectFit
    chevron.isUserInteractionEnabled = false
    chevron.widthAnchor.constraint(equalToConstant: 12).isActive = true
    chevron.heightAnchor.constraint(equalToConstant: 18).isActive = true

    let stack = UIStackView(arrangedSubviews: [labels, chevron])
    stack.axis = .horizontal
    stack.spacing = 12
    stack.alignment = .center
    stack.translatesAutoresizingMaskIntoConstraints = false
    stack.isUserInteractionEnabled = false

    let control = UIControl()
    control.isAccessibilityElement = true
    control.accessibilityLabel = title
    control.accessibilityHint = subtitle
    control.accessibilityTraits.insert(.button)
    control.translatesAutoresizingMaskIntoConstraints = false
    control.addSubview(stack)
    labels.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
    NSLayoutConstraint.activate([
        control.heightAnchor.constraint(greaterThanOrEqualToConstant: 52),
        stack.leadingAnchor.constraint(equalTo: control.leadingAnchor),
        stack.trailingAnchor.constraint(equalTo: control.trailingAnchor),
        stack.topAnchor.constraint(equalTo: control.topAnchor, constant: 6),
        stack.bottomAnchor.constraint(equalTo: control.bottomAnchor, constant: -6)
    ])
    return control
}

func statusText(_ status: NEVPNStatus) -> String {
    switch status {
    case .connected: return L10n.tr("vpn.connected")
    case .connecting: return L10n.tr("vpn.connecting")
    case .disconnecting: return L10n.tr("vpn.disconnecting")
    case .reasserting: return L10n.tr("vpn.reasserting")
    case .invalid: return L10n.tr("vpn.invalid")
    case .disconnected: return L10n.tr("vpn.disconnected")
    @unknown default: return L10n.tr("vpn.unknown")
    }
}

extension TelemetrySettings.Destination {
    var localizedDisplayName: String {
        switch self {
        case .developer:
            return L10n.tr("telemetry.destination.developer")
        case .custom:
            return L10n.tr("telemetry.destination.custom")
        }
    }
}

extension LaunchRouteMode {
    var localizedDisplayName: String {
        switch self {
        case .geo:
            return L10n.tr("home.route.geo")
        case .global:
            return L10n.tr("home.route.global")
        case .basic:
            return L10n.tr("home.route.basic")
        }
    }
}

extension TelemetryUploadSummary {
    var localizedDisplayText: String {
        if skipped > 0 && attempted == 0 {
            return L10n.tr("telemetry.summarySkipped")
        }
        if failed == 0 {
            return uploaded == 0
                ? L10n.tr("telemetry.summaryNoReports")
                : L10n.format("telemetry.summaryUploaded", uploaded)
        }
        return L10n.format("telemetry.summaryUploadedFailed", uploaded, failed)
    }
}

extension CrashReporter.ProcessKind {
    var localizedDisplayName: String {
        switch self {
        case .app:
            return L10n.tr("crash.process.app")
        case .packetTunnel:
            return L10n.tr("crash.process.packetTunnel")
        }
    }
}

func localizedCrashReportSummary(for snapshots: [CrashReporter.StoreSnapshot]) -> String {
    let count = snapshots.reduce(0) { $0 + $1.reportCount }
    if count == 0 {
        let summary = CrashReporter.crashedLastLaunch ? L10n.tr("crash.generating") : L10n.tr("crash.none")
        return snapshots.contains { $0.process == .packetTunnel } ? summary : L10n.format("crash.partial", summary)
    }

    let processSummary = snapshots
        .filter { $0.reportCount > 0 }
        .map { "\($0.process.localizedDisplayName) \($0.reportCount)" }
        .joined(separator: " / ")
    return processSummary.isEmpty
        ? L10n.format("crash.pending", count)
        : L10n.format("crash.pendingWithProcess", count, processSummary)
}

func localizedCrashStorageDescription() -> String {
    CrashReporter.isSharedContainerAvailable ? L10n.tr("crash.appGroup") : L10n.tr("crash.localCache")
}

extension UIViewController {
    func confirmSensitiveAction(title: String, message: String, action: String, perform: @escaping () -> Void) {
        let alert = UIAlertController(title: title, message: message, preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: L10n.tr("common.cancel"), style: .cancel))
        alert.addAction(UIAlertAction(title: action, style: .default) { _ in perform() })
        present(alert, animated: true)
    }

    func confirmReportUpload(to endpoint: String, savesSettings: Bool, perform: @escaping () -> Void) {
        var message = L10n.format("crash.confirmUpload", endpoint)
        if savesSettings { message += "\n\n" + L10n.tr("crash.savesSettings") }
        confirmSensitiveAction(title: L10n.tr("telemetry.uploadCrashReports"), message: message, action: L10n.tr("telemetry.upload"), perform: perform)
    }

    func presentMessage(title: String, message: String) {
        let alert = UIAlertController(title: title, message: message, preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: L10n.tr("common.close"), style: .default))
        present(alert, animated: true)
    }

    func presentToast(_ message: String) {
        let alert = UIAlertController(title: nil, message: message, preferredStyle: .alert)
        present(alert, animated: true)
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) {
            alert.dismiss(animated: true)
        }
    }

    func promptRestartForActiveTunnelIfNeeded(
        profile: ConfigProfile,
        message: String,
        noRestartMessage: String?,
        completion: (() -> Void)? = nil
    ) {
        let vpn = VPNController.shared
        guard vpn.requiresRestartToApplyConfiguration else {
            if let noRestartMessage {
                presentToast(noRestartMessage)
            }
            completion?()
            return
        }

        let alert = UIAlertController(title: L10n.tr("restart.title"), message: message, preferredStyle: .alert)
        alert.addAction(UIAlertAction(title: L10n.tr("restart.later"), style: .cancel) { _ in
            completion?()
        })
        alert.addAction(UIAlertAction(title: L10n.tr("restart.now"), style: .default) { [weak self] _ in
            vpn.reconnect(profile: profile) { result in
                switch result {
                case .success:
                    self?.presentToast(L10n.tr("restart.success"))
                    completion?()
                case let .failure(error):
                    self?.presentMessage(title: L10n.tr("restart.failed"), message: error.localizedDescription)
                }
            }
        })
        present(alert, animated: true)
    }
}

// MARK: - Secure subscription fetch

func isSecureSubscriptionURL(_ url: URL) -> Bool {
    SubscriptionUrlPolicy.isSecure(url)
}

final class SecureSubscriptionRedirectDelegate: NSObject, URLSessionTaskDelegate {
    private let lock = NSLock()
    private var redirectCounts: [ObjectIdentifier: Int] = [:]

    func urlSession(
        _ session: URLSession,
        task: URLSessionTask,
        willPerformHTTPRedirection response: HTTPURLResponse,
        newRequest request: URLRequest,
        completionHandler: @escaping (URLRequest?) -> Void
    ) {
        let id = ObjectIdentifier(task)
        lock.lock()
        let count = (redirectCounts[id] ?? 0) + 1
        redirectCounts[id] = count
        lock.unlock()

        guard count <= SubscriptionUrlPolicy.maxRedirects else {
            completionHandler(nil)
            return
        }
        guard let url = request.url, SubscriptionUrlPolicy.isSecure(url) else {
            completionHandler(nil)
            return
        }
        completionHandler(request)
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        let id = ObjectIdentifier(task)
        lock.lock()
        redirectCounts.removeValue(forKey: id)
        lock.unlock()
    }
}

private let secureSubscriptionRedirectDelegate = SecureSubscriptionRedirectDelegate()
let secureSubscriptionSession = URLSession(
    configuration: .default,
    delegate: secureSubscriptionRedirectDelegate,
    delegateQueue: nil
)
