import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import '../models/app_scope.dart';
import '../models/config_profile.dart';
import '../models/launch_route_mode.dart';
import '../runtime/runtime_controls.dart';
import '../runtime/runtime_snapshot.dart';
import '../runtime/runtime_store.dart';
import '../runtime/runtime_traffic_rate.dart';
import '../services/profile_store.dart';
import '../services/telemetry_settings_store.dart';
import '../vpn_service.dart';
import '../widgets/debug_panel.dart';
import '../widgets/profile_ui.dart';
import 'profile_edit_page.dart';
import 'profiles_page.dart' show importProfileSubscription;

class HomePage extends StatefulWidget {
  const HomePage({super.key});

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> with WidgetsBindingObserver {
  final _vpnService = VpnService();
  final _store = ProfileStore();
  late final RuntimeStore _runtimeStore;

  List<ConfigProfile> _profiles = const [];
  ConfigProfile? _active;
  bool _profilesLoaded = false;
  String _savedScope = '';
  Map<String, dynamic> _launchOptions = Map<String, dynamic>.from(
    ProfileStore.defaultOptions,
  );
  String? _lastError;
  bool _permissionDenied = false;
  bool _debugPanelEnabled = false;
  String _debugLog = '';
  String _logPath = '';
  int? _pendingStartGeneration;
  int? _pendingStopGeneration;

  Timer? _connectWatchdogTimer;
  Timer? _stopPresentationTimer;
  Timer? _logPollTimer;
  DateTime? _stopStartedAt;

  static const _stopPresentationTimeout = Duration(seconds: 15);

  StreamSubscription<String>? _errorSub;
  StreamSubscription<void>? _storeSub;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    _vpnService.init();
    _runtimeStore = _vpnService.runtimeStore;
    _runtimeStore.addListener(_runtimeChanged);
    _errorSub = _vpnService.errorStream.listen((error) {
      if (!mounted) return;
      _connectWatchdogTimer?.cancel();
      setState(() => _lastError = error);
      unawaited(_showErrorDialog(error));
    });
    _storeSub = _store.changes.listen((_) => _refreshStore());

    unawaited(_refreshStore());
    unawaited(_loadDebugPanelEnabled());
  }

  void _runtimeChanged() {
    if (!mounted) return;
    final phase = _runtimeStore.state.phase;
    if (phase == RuntimePhase.stopping) {
      _stopStartedAt ??= DateTime.now();
      _stopPresentationTimer ??=
          Timer.periodic(const Duration(seconds: 1), (_) {
        if (mounted) setState(() {});
      });
    } else {
      _stopStartedAt = null;
      _stopPresentationTimer?.cancel();
      _stopPresentationTimer = null;
    }
    setState(() {
      final pending = _pendingStartGeneration;
      if (pending != null &&
          (_runtimeStore.state.generation > pending ||
              phase == RuntimePhase.unknown ||
              phase == RuntimePhase.failed ||
              phase == RuntimePhase.connected)) {
        _pendingStartGeneration = null;
        _vpnService.connecting = false;
      }
      final pendingStop = _pendingStopGeneration;
      if (pendingStop != null &&
          (_runtimeStore.state.generation > pendingStop ||
              phase == RuntimePhase.idle ||
              phase == RuntimePhase.failed)) {
        _pendingStopGeneration = null;
      }
      if (phase == RuntimePhase.connected) {
        _connectWatchdogTimer?.cancel();
        _vpnService.connecting = false;
      }
    });
  }

  Future<void> _refreshStore() async {
    final profiles = await _store.getProfiles();
    final active = await _store.getActive();
    Map<String, dynamic> options = Map<String, dynamic>.from(
      ProfileStore.defaultOptions,
    );
    if (active != null) {
      options = await _store.getProfileOptions(active.id);
    }
    if (!mounted) return;
    setState(() {
      _profilesLoaded = true;
      _profiles = profiles;
      _active = active;
      _launchOptions = options;
      final apps = options['perAppProxyApps'];
      _savedScope = appScopeSummary(
        enabled: options['perAppProxyEnabled'] == true,
        mode: options['perAppProxyMode']?.toString() ?? 'allow',
        count: apps is List
            ? apps.whereType<String>().where((p) => p.trim().isNotEmpty).toSet().length
            : 0,
      );
    });
  }

  @override
  void didChangeAppLifecycleState(AppLifecycleState state) {
    if (state == AppLifecycleState.resumed) {
      unawaited(_refreshStore());
      unawaited(_loadDebugPanelEnabled());
    }
  }

  Future<void> _loadDebugPanelEnabled() async {
    final enabled = await _store.getDebugPanelEnabled();
    if (!mounted) return;
    setState(() => _debugPanelEnabled = enabled);
    if (enabled) {
      _startLogPolling();
      unawaited(_refreshDebugInfo());
    } else {
      _logPollTimer?.cancel();
      _logPollTimer = null;
    }
  }

  void _startLogPolling() {
    _logPollTimer?.cancel();
    _logPollTimer = Timer.periodic(const Duration(seconds: 2), (_) {
      unawaited(_refreshDebugInfo());
    });
  }

  /// Elapsed connect time comes from the runtime snapshot's own clock, so it
  /// stays correct after the UI process is killed and recreated while `:vpn`
  /// keeps running.
  String get _duration => _formatDuration(
        Duration(milliseconds: connectedElapsedMs(_runtimeStore.state)),
      );

  String _formatDuration(Duration d) {
    final h = d.inHours.toString().padLeft(2, '0');
    final m = (d.inMinutes % 60).toString().padLeft(2, '0');
    final s = (d.inSeconds % 60).toString().padLeft(2, '0');
    return '$h:$m:$s';
  }

  String _formatBytes(int bytes) {
    if (bytes < 1024) return '$bytes B';
    if (bytes < 1024 * 1024) return '${(bytes / 1024).toStringAsFixed(1)} KB';
    if (bytes < 1024 * 1024 * 1024) {
      return '${(bytes / (1024 * 1024)).toStringAsFixed(1)} MB';
    }
    return '${(bytes / (1024 * 1024 * 1024)).toStringAsFixed(2)} GB';
  }

  String _formatSpeed(int bps) => '${_formatBytes(bps)}/s';

  Future<void> _toggleConnection() async {
    switch (controlsFor(_runtimeStore.state.phase).action) {
      case RuntimeConnectionAction.cancel:
      case RuntimeConnectionAction.stop:
      case RuntimeConnectionAction.forceStop:
        await _stopVpnForDebug();
        return;
      case RuntimeConnectionAction.none:
        return;
      case RuntimeConnectionAction.start:
      case RuntimeConnectionAction.retry:
        if (_pendingStartGeneration != null) return;
        break;
    }

    final profile = _active;
    if (profile == null || profile.json.trim().isEmpty) {
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('请先添加并选择一个配置')),
      );
      return;
    }
    setState(() {
      _permissionDenied = false;
      _lastError = null;
      _pendingStartGeneration = _runtimeStore.state.generation;
    });
    _vpnService.connecting = true;
    try {
      await _vpnService.clearLog();
      final options = await _store.getProfileOptions(profile.id);
      final telemetry = await TelemetrySettingsStore().settings();
      final mergedJson = ProfileStore.effectiveJson(
        profile.json,
        options,
        telemetry: telemetry,
      );
      final accepted = await _vpnService.connect(
        mergedJson,
        vpnOptions: options,
      );
      if (!accepted) {
        throw StateError('VPN start command was rejected');
      }
      // Drop any previous session watermark so the first mirrored snapshot
      // from the new `:vpn` generation is accepted even when its counter
      // restarts at 1.
      _runtimeStore.endSession();
      _startConnectWatchdog();
      if (mounted) setState(() {});
    } catch (e) {
      if (!mounted) return;
      _vpnService.connecting = false;
      final denied = e is PlatformException && e.code == 'PERMISSION_DENIED';
      final error = denied ? '未获得系统 VPN 授权，无法连接。' : e.toString();
      setState(() {
        _pendingStartGeneration = null;
        _lastError = error;
        _permissionDenied = denied;
      });
      if (denied) {
        await _showPermissionDenied();
      } else {
        await _showErrorDialog(error);
      }
    }
  }

  static const int _connectMaxSeconds = 180;

  void _startConnectWatchdog() {
    _connectWatchdogTimer?.cancel();
    final startedAt = DateTime.now();
    _connectWatchdogTimer =
        Timer.periodic(const Duration(seconds: 5), (timer) async {
      if (!mounted) {
        timer.cancel();
        return;
      }
      final phase = _runtimeStore.state.phase;
      if (phase == RuntimePhase.connected) {
        timer.cancel();
        return;
      }
      if (_pendingStartGeneration == null &&
          controlsFor(phase).action != RuntimeConnectionAction.cancel) {
        timer.cancel();
        return;
      }
      final hbAgeMs = await _vpnService.getVpnHeartbeatAgeMs();
      final hbStale = hbAgeMs < 0 || hbAgeMs > 30000;
      final totalSec = DateTime.now().difference(startedAt).inSeconds;
      if (!hbStale && totalSec < _connectMaxSeconds) return;
      timer.cancel();
      _vpnService.connecting = false;
      final reason = totalSec >= _connectMaxSeconds
          ? '超过 ${_connectMaxSeconds}s 上限'
          : ':vpn 心跳已停 ${(hbAgeMs / 1000).toStringAsFixed(1)}s';
      final timedOutPhase = _runtimeStore.state.phase;
      final error = timedOutPhase == RuntimePhase.unknown
          ? '连接超时（$reason）：VPN Service 没有发布运行时状态。'
          : timedOutPhase == RuntimePhase.handshaking ||
                  timedOutPhase == RuntimePhase.applyingPolicy
              ? '连接超时（$reason）：native 引擎已启动但未完成握手。\n请检查所选配置的服务器地址、密钥与网络连通性。'
              : '连接超时（$reason）：VPN 未进入已连接状态。';
      if (!mounted ||
          (_pendingStartGeneration == null &&
              controlsFor(_runtimeStore.state.phase).action !=
                  RuntimeConnectionAction.cancel)) {
        return;
      }
      setState(() {
        _pendingStartGeneration = null;
        _lastError = error;
      });
      await _vpnService.disconnect();
      await _showErrorDialog(error);
    });
  }

  Future<void> _showPermissionDenied() async {
    final retry = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('未获得 VPN 授权'),
        content: const Text('连接需要 Android 建立 VPN 的权限。点击重试，在系统连接请求中选择允许。'),
        actions: [
          TextButton(onPressed: () => Navigator.of(ctx).pop(false), child: const Text('取消')),
          FilledButton(onPressed: () => Navigator.of(ctx).pop(true), child: const Text('重试授权')),
        ],
      ),
    );
    if (retry != true || !mounted) return;
    final action = controlsFor(_runtimeStore.state.phase).action;
    if (action == RuntimeConnectionAction.start || action == RuntimeConnectionAction.retry) {
      await _toggleConnection();
    }
  }

  Future<void> _showErrorDialog(String error) async {
    if (!mounted) return;
    final log = await _vpnService.readLog();
    final logPath = await _vpnService.getLogPath();
    final details = [
      '错误:',
      error,
      '',
      '日志文件:',
      logPath,
      '',
      '日志内容:',
      log.isEmpty ? '(暂无日志)' : log,
    ].join('\n');
    if (!mounted) return;
    await showDialog<void>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('连接失败'),
        content: SizedBox(
          width: double.maxFinite,
          child: SingleChildScrollView(child: SelectableText(details)),
        ),
        actions: [
          TextButton(
            onPressed: () async {
              await Clipboard.setData(ClipboardData(text: details));
              if (ctx.mounted) {
                ScaffoldMessenger.of(ctx).showSnackBar(
                  const SnackBar(content: Text('错误信息已复制')),
                );
              }
            },
            child: const Text('复制错误'),
          ),
          TextButton(
            onPressed: () => Navigator.of(ctx).pop(),
            child: const Text('关闭'),
          ),
        ],
      ),
    );
  }

  Future<void> _refreshDebugInfo() async {
    final log = await _vpnService.readLog();
    final path = await _vpnService.getLogPath();
    if (!mounted) return;
    setState(() {
      _debugLog = log;
      _logPath = path;
    });
  }

  Future<void> _copyDebugInfo() async {
    final text = [
      'OpenPPP2 调试信息',
      '状态: ${_getStateText()}',
      '日志文件: $_logPath',
      '',
      _debugLog.isEmpty ? '(暂无日志)' : _debugLog,
    ].join('\n');
    await Clipboard.setData(ClipboardData(text: text));
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('调试信息已复制')),
    );
  }

  Future<void> _clearDebugLog() async {
    await _vpnService.clearLog();
    await _refreshDebugInfo();
  }

  Future<void> _stopVpnForDebug() async {
    final generation = _runtimeStore.state.generation;
    if (_pendingStopGeneration == generation) return;
    final forceStop = _runtimeStore.state.phase == RuntimePhase.unknown;
    _connectWatchdogTimer?.cancel();
    _vpnService.connecting = false;
    if (mounted) {
      setState(() {
        _pendingStartGeneration = null;
        _pendingStopGeneration = generation;
      });
    } else {
      _pendingStartGeneration = null;
      _pendingStopGeneration = generation;
    }
    try {
      final accepted = await _vpnService.disconnect();
      if (!accepted) {
        throw StateError('VPN stop command was rejected');
      }
      if (forceStop && mounted) {
        setState(() => _pendingStopGeneration = null);
      }
    } catch (e) {
      if (!mounted) return;
      final error = e.toString();
      setState(() {
        _pendingStopGeneration = null;
        _lastError = error;
      });
      await _showErrorDialog(error);
    }
  }

  Future<void> _applyProfile(ConfigProfile profile) async {
    if (profile.id == _active?.id) return;
    await _store.setActive(profile.id);
    await _refreshStore();
    if (!mounted) return;
    if (!controlsFor(_runtimeStore.state.phase).configEditable) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('已切换到「${profile.name}」，重连后生效')),
      );
    }
  }

  Future<void> _editProfile(ConfigProfile profile) async {
    final ok = await Navigator.of(context).push<bool>(
      MaterialPageRoute(builder: (_) => ProfileEditPage(profile: profile)),
    );
    if (ok == true) await _refreshStore();
  }

  Future<void> _addProfile() async {
    final ok = await Navigator.of(context).push<bool>(
      MaterialPageRoute(builder: (_) => const ProfileEditPage()),
    );
    if (ok == true) await _refreshStore();
  }

  Future<void> _togglePin(ConfigProfile profile) async {
    await _store.toggleFavorite(profile.id);
    await _refreshStore();
  }

  Future<void> _updateLaunchOption(
    void Function(Map<String, dynamic> options) mutate,
  ) async {
    final active = _active;
    if (active == null) return;
    await _store.updateProfileOptions(active.id, mutate);
    if (!mounted) return;
    final next = await _store.getProfileOptions(active.id);
    if (!mounted) return;
    setState(() => _launchOptions = next);
    if (!controlsFor(_runtimeStore.state.phase).configEditable) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('快捷设置已保存，重连后生效')),
      );
    }
  }

  String _getStateText() {
    return controlsFor(_runtimeStore.state.phase).statusLabel;
  }

  bool get _stopTakingTooLong {
    final startedAt = _stopStartedAt;
    return startedAt != null &&
        DateTime.now().difference(startedAt) >= _stopPresentationTimeout;
  }

  @override
  void dispose() {
    WidgetsBinding.instance.removeObserver(this);
    _errorSub?.cancel();
    _storeSub?.cancel();
    _runtimeStore.removeListener(_runtimeChanged);
    _connectWatchdogTimer?.cancel();
    _stopPresentationTimer?.cancel();
    _logPollTimer?.cancel();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final phase = _runtimeStore.state.phase;
    final startPending = _pendingStartGeneration != null &&
        (phase == RuntimePhase.idle ||
            phase == RuntimePhase.failed ||
            phase == RuntimePhase.unknown);
    final controls = controlsFor(
      startPending ? RuntimePhase.starting : phase,
      stopTakingTooLong: _stopTakingTooLong,
    );
    final commandPending = _pendingStartGeneration != null ||
        _pendingStopGeneration == _runtimeStore.state.generation;
    final configEditable = controls.configEditable && !commandPending;
    final statusTitle = controls.statusLabel;
    final statusDetail =
        controls.isConnected ? _duration : controls.detailLabel;

    final routeMode = LaunchRouteMode.fromOptions(_launchOptions);
    final active = _active;
    final canStart = controls.action == RuntimeConnectionAction.start ||
        controls.action == RuntimeConnectionAction.retry;
    final seedOnly = _profiles.length == 1 && active?.name == 'Default' &&
        active?.json == ProfileStore.defaultJson && active?.subscriptionUrl == null;
    final needsSetup = active == null || active.json.trim().isEmpty;
    final showSetup = _profilesLoaded && canStart &&
        (needsSetup || (seedOnly && _lastError == null));
    final retryPermission = _permissionDenied && canStart && !showSetup;
    final savedRoute = _launchOptions['proxyOnly'] == true
        ? '仅代理模式 · 本机 HTTP/SOCKS，不接管常规 VPN 路由'
        : '${routeMode.label} · IPv4 路由 ${_launchOptions['route']}/${_launchOptions['routePrefix']}';

    return Scaffold(
      appBar: AppBar(
        title: const Text('OPENPPP2'),
        centerTitle: true,
      ),
      body: SafeArea(
        child: ListView(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 24),
          children: [
            HomeStatusCard(
              statusText: statusTitle,
              detailText: showSetup
                  ? (needsSetup ? '导入订阅或手动添加配置后即可连接' : '可导入自己的节点，也可使用默认配置')
                  : retryPermission ? '需要系统 VPN 授权' : statusDetail,
              isConnected: controls.isConnected,
              isBusy: controls.isBusy,
              buttonLabel: showSetup ? '导入远程订阅' : retryPermission ? '重试授权' : controls.buttonLabel,
              buttonEnabled: controls.buttonEnabled && !commandPending && (!canStart || _profilesLoaded),
              uploadText: _formatSpeed(_vpnService.traffic.txBytesPerSecond),
              downloadText: _formatSpeed(_vpnService.traffic.rxBytesPerSecond),
              allowLan: _launchOptions['allowLan'] == true,
              blockQuic: _launchOptions['blockQuic'] == true,
              routeMode: routeMode,
              onConnect: showSetup
                  ? () => importProfileSubscription(context, _store, selectFirst: true)
                  : _toggleConnection,
              onAllowLanChanged: _active == null || !configEditable
                  ? null
                  : (v) => _updateLaunchOption((o) {
                        o['allowLan'] = v;
                      }),
              onBlockQuicChanged: _active == null || !configEditable
                  ? null
                  : (v) => _updateLaunchOption((o) {
                        o['blockQuic'] = v;
                      }),
              onRouteModeChanged: _active == null || !configEditable
                  ? null
                  : (mode) => _updateLaunchOption((o) {
                        final next = LaunchRouteMode.applyTo(o, mode);
                        o
                          ..clear()
                          ..addAll(next);
                      }),
            ),
            if (showSetup)
              Wrap(
                alignment: WrapAlignment.center,
                spacing: 12,
                children: [
                  TextButton(onPressed: _addProfile, child: const Text('手动添加配置')),
                  if (!needsSetup)
                    TextButton(onPressed: _toggleConnection, child: const Text('连接默认配置')),
                ],
              ),
            if (active != null)
              Padding(
                padding: const EdgeInsets.fromLTRB(8, 12, 8, 0),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text('已选配置：${active.name}', style: theme.textTheme.titleSmall),
                    Text('已保存的应用范围：$_savedScope'),
                    Text('已保存的路由：$savedRoute'),
                  ],
                ),
              ),
            if (controls.isConnected)
              ExpansionTile(
                key: const PageStorageKey<String>('effective-connection-details'),
                title: const Text('当前连接详情'),
                expandedAlignment: Alignment.centerLeft,
                expandedCrossAxisAlignment: CrossAxisAlignment.start,
                childrenPadding: const EdgeInsets.fromLTRB(16, 0, 16, 12),
                children: [
                  if (_runtimeStore.state.effectiveMuxMode.isNotEmpty)
                    Text('VMUX: ${_runtimeStore.state.effectiveMuxDisplayName}'),
                  Text('Path: ${_runtimeStore.state.effectivePathDisplayName}'),
                ],
              ),
            const SizedBox(height: 16),
            Row(
              children: [
                Text(
                  '配置文件',
                  style: theme.textTheme.titleSmall?.copyWith(
                    color: theme.colorScheme.onSurfaceVariant,
                    fontWeight: FontWeight.w700,
                  ),
                ),
                const Spacer(),
                if (!showSetup)
                  IconButton(
                    icon: const Icon(Icons.add_circle_outline),
                    tooltip: '手动添加配置',
                    onPressed: configEditable ? _addProfile : null,
                  ),
              ],
            ),
            const SizedBox(height: 6),
            GroupedProfileList(
              profiles: _profiles,
              activeId: _active?.id,
              shrinkWrap: true,
              maxHeight: MediaQuery.sizeOf(context).height * 0.38,
              onTap: configEditable ? _applyProfile : null,
              onApply: configEditable ? _applyProfile : null,
              onEdit: configEditable ? _editProfile : null,
              onTogglePin: configEditable ? _togglePin : null,
            ),
            if (_lastError != null) ...[
              const SizedBox(height: 12),
              Card(
                color: theme.colorScheme.errorContainer,
                child: Padding(
                  padding: const EdgeInsets.all(12),
                  child: Text(
                    _lastError!,
                    style: theme.textTheme.bodySmall?.copyWith(
                      color: theme.colorScheme.onErrorContainer,
                    ),
                  ),
                ),
              ),
            ],
            if (_debugPanelEnabled) ...[
              const SizedBox(height: 16),
              DebugPanel(
                stateText: _getStateText(),
                logPath: _logPath,
                logText: _debugLog,
                onRefresh: _refreshDebugInfo,
                onCopy: _copyDebugInfo,
                onClear: _clearDebugLog,
                onStop: _stopVpnForDebug,
                runtimeSnapshot: _runtimeStore.state,
              ),
            ],
          ],
        ),
      ),
    );
  }
}

bool mapEquals(Map<String, dynamic> a, Map<String, dynamic> b) {
  if (a.length != b.length) return false;
  for (final key in a.keys) {
    if (!b.containsKey(key)) return false;
    final av = a[key];
    final bv = b[key];
    if (av is Map && bv is Map) {
      if (!mapEquals(
        Map<String, dynamic>.from(av),
        Map<String, dynamic>.from(bv),
      )) {
        return false;
      }
    } else if (av != bv) {
      return false;
    }
  }
  return true;
}
