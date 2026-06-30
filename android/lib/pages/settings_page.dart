import 'dart:async';
import 'dart:convert';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import '../services/profile_store.dart';
import '../services/theme_controller.dart';
import '../vpn_service.dart';
import '../widgets/debug_panel.dart';

class SettingsPage extends StatefulWidget {
  const SettingsPage({super.key});

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  final _store = ProfileStore();
  final _vpnService = VpnService();
  final _otelEndpoint = TextEditingController();
  final _otelLogFile = TextEditingController();

  bool _debugPanelEnabled = false;
  bool _otelEnabled = false;
  bool _otelMetricsEnabled = false;
  bool _otelSpansEnabled = false;
  bool _otelConsoleLog = true;
  bool _otelConsoleMetric = false;
  bool _otelConsoleSpan = false;
  bool _otelDirty = false;
  int _otelLevel = 1;
  bool _loading = true;
  VpnState _state = VpnState.disconnected;
  String _debugLog = '';
  String _logPath = '';
  String _pathAwareness = '{}';
  int _pathAwarenessAgeMs = -1;
  Timer? _pollTimer;
  Timer? _pathPollTimer;
  StreamSubscription<VpnState>? _stateSub;
  StreamSubscription<String>? _pathSub;

  @override
  void initState() {
    super.initState();
    _vpnService.init();
    _stateSub = _vpnService.stateStream.listen((s) {
      if (!mounted) return;
      setState(() => _state = s);
    });
    _pathSub = _vpnService.pathAwarenessStream.listen((snapshot) {
      if (!mounted) return;
      setState(() => _pathAwareness = snapshot);
    });
    _load();
  }

  Future<void> _load() async {
    final enabled = await _store.getDebugPanelEnabled();
    final telemetry = await _store.getTelemetrySettings();
    if (!mounted) return;
    _hydrateTelemetry(telemetry);
    setState(() {
      _debugPanelEnabled = enabled;
      _otelDirty = false;
      _loading = false;
    });
    unawaited(_refresh());
    _startPathPoll();
    if (enabled) {
      _startPoll();
    }
  }

  void _startPoll() {
    _pollTimer?.cancel();
    _pollTimer = Timer.periodic(const Duration(seconds: 2), (_) {
      unawaited(_refresh());
    });
  }

  void _startPathPoll() {
    _pathPollTimer?.cancel();
    _pathPollTimer = Timer.periodic(const Duration(seconds: 2), (_) {
      unawaited(_refreshPathAwareness());
    });
  }

  Future<void> _refresh() async {
    final state = await _vpnService.getState();
    final log = await _vpnService.readLog();
    final path = await _vpnService.getLogPath();
    final pathAwareness = await _vpnService.getPathAwareness();
    final pathAwarenessAgeMs = await _vpnService.getPathAwarenessAgeMs();
    if (!mounted) return;
    setState(() {
      _state = state;
      _debugLog = log;
      _logPath = path;
      _pathAwareness = pathAwareness;
      _pathAwarenessAgeMs = pathAwarenessAgeMs;
    });
  }

  Future<void> _refreshPathAwareness() async {
    final pathAwareness = await _vpnService.getPathAwareness();
    final pathAwarenessAgeMs = await _vpnService.getPathAwarenessAgeMs();
    if (!mounted) return;
    setState(() {
      _pathAwareness = pathAwareness;
      _pathAwarenessAgeMs = pathAwarenessAgeMs;
    });
  }

  Future<void> _setDebugPanel(bool v) async {
    setState(() => _debugPanelEnabled = v);
    await _store.setDebugPanelEnabled(v);
    if (v) {
      _startPoll();
      unawaited(_refresh());
    } else {
      _pollTimer?.cancel();
      _pollTimer = null;
    }
  }

  void _hydrateTelemetry(Map<String, dynamic> settings) {
    _otelEnabled = settings['enabled'] == true;
    _otelEndpoint.text = (settings['endpoint'] ?? '').toString();
    _otelLogFile.text = (settings['logFile'] ?? '').toString();
    _otelMetricsEnabled = settings['metricsEnabled'] == true;
    _otelSpansEnabled = settings['spansEnabled'] == true;
    _otelConsoleLog = settings['consoleLog'] != false;
    _otelConsoleMetric = settings['consoleMetric'] == true;
    _otelConsoleSpan = settings['consoleSpan'] == true;
    _otelLevel = (int.tryParse((settings['level'] ?? '1').toString()) ?? 1)
        .clamp(0, 3)
        .toInt();
  }

  Map<String, dynamic> _readTelemetryForm() => {
        'enabled': _otelEnabled,
        'endpoint': _otelEndpoint.text.trim(),
        'level': _otelLevel,
        'metricsEnabled': _otelMetricsEnabled,
        'spansEnabled': _otelSpansEnabled,
        'logFile': _otelLogFile.text.trim(),
        'consoleLog': _otelConsoleLog,
        'consoleMetric': _otelConsoleMetric,
        'consoleSpan': _otelConsoleSpan,
      };

  void _markTelemetryDirty() {
    setState(() => _otelDirty = true);
  }

  void _updateTelemetry(VoidCallback change) {
    setState(() {
      change();
      _otelDirty = true;
    });
  }

  Future<void> _saveTelemetry() async {
    await _store.setTelemetrySettings(_readTelemetryForm());
    if (!mounted) return;
    setState(() => _otelDirty = false);
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('OpenTelemetry 设置已保存，重连后生效')),
    );
  }

  void _resetTelemetry() {
    setState(() {
      _hydrateTelemetry(
        Map<String, dynamic>.from(ProfileStore.defaultTelemetrySettings),
      );
      _otelDirty = true;
    });
  }

  Future<void> _copyLog() async {
    await Clipboard.setData(ClipboardData(
      text: _debugLog.isEmpty ? '(暂无日志)' : _debugLog,
    ));
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('日志已复制')),
    );
  }

  Future<void> _clearLog() async {
    await _vpnService.clearLog();
    await _refresh();
  }

  Future<void> _stopVpn() async {
    await _vpnService.disconnect();
    await _refresh();
  }

  String _stateText() {
    switch (_state) {
      case VpnState.connected:
        return '已连接';
      case VpnState.connecting:
        return '连接中';
      case VpnState.disconnecting:
        return '断开中';
      case VpnState.disconnected:
        return '未连接';
    }
  }

  String _telemetrySummary() {
    if (!_otelEnabled) return '关闭';
    final endpoint = _otelEndpoint.text.trim();
    if (endpoint.isEmpty && _otelLogFile.text.trim().isEmpty) {
      return '启用 · Console 输出';
    }
    if (endpoint.isEmpty) return '启用 · 本地文件输出';
    return '启用 · $endpoint';
  }

  String _levelLabel(int level) {
    switch (level) {
      case 0:
        return 'Info';
      case 1:
        return 'Verbose';
      case 2:
        return 'Debug';
      case 3:
        return 'Trace';
      default:
        return 'Verbose';
    }
  }

  Map<String, dynamic> _pathAwarenessMap() {
    try {
      final decoded = jsonDecode(_pathAwareness);
      return decoded is Map ? Map<String, dynamic>.from(decoded) : {};
    } catch (_) {
      return {};
    }
  }

  List<Map<String, dynamic>> _pathList(Map<String, dynamic> root) {
    final local = root['local'];
    if (local is! Map) return const [];
    final paths = local['paths'];
    if (paths is! List) return const [];
    return paths
        .whereType<Map>()
        .map((e) => Map<String, dynamic>.from(e))
        .toList(growable: false);
  }

  String _pathAwarenessSummary(Map<String, dynamic> root) {
    if (root.isEmpty) return '暂无快照';
    final policy = root['policy'];
    final policyMap = policy is Map ? Map<String, dynamic>.from(policy) : {};
    final enabled = policyMap['pathAwarenessEnabled'] != false;
    final paths = _pathList(root);
    final active = paths.where((p) => p['available'] == true).length;
    final mode = policyMap['mode']?.toString() ?? 'observe';
    final age = _pathAwarenessAgeMs < 0 ? '未知' : '${_pathAwarenessAgeMs}ms';
    if (!enabled) return '关闭 · $age';
    return '$active 条可用路径 · $mode · $age';
  }

  String _pathLabel(Map<String, dynamic> path) {
    final transport = path['transport']?.toString() ?? 'unknown';
    final score = path['score']?.toString() ?? '-';
    final validated = path['validated'] == true ? 'validated' : 'unvalidated';
    final metered = path['metered'] == true ? 'metered' : 'unmetered';
    return '$transport · score $score · $validated · $metered';
  }

  Widget _pathAwarenessCard(ThemeData theme) {
    final root = _pathAwarenessMap();
    final paths = _pathList(root);
    final local = root['local'];
    final localMap = local is Map ? Map<String, dynamic>.from(local) : {};
    final native = root['native'];
    final nativeMap = native is Map ? Map<String, dynamic>.from(native) : {};
    final observations = nativeMap['observations'];
    final observationCount = observations is List ? observations.length : 0;
    final primary = localMap['primaryTransport']?.toString();

    return Card(
      child: Padding(
        padding: const EdgeInsets.only(bottom: 8),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            ListTile(
              leading: const Icon(Icons.hub_rounded),
              title: const Text('路径感知'),
              subtitle: Text(_pathAwarenessSummary(root)),
              trailing: IconButton(
                tooltip: '刷新',
                onPressed: _refresh,
                icon: const Icon(Icons.refresh_rounded),
              ),
            ),
            const Divider(height: 0),
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 10, 16, 4),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: [
                  Text(
                    'Primary: ${primary == null || primary == 'null' ? '(无)' : primary} · Native observations: $observationCount',
                    style: theme.textTheme.bodySmall?.copyWith(
                      color: theme.colorScheme.onSurfaceVariant,
                    ),
                  ),
                  const SizedBox(height: 8),
                  if (paths.isEmpty)
                    Text(
                      '(暂无路径快照)',
                      style: theme.textTheme.bodySmall?.copyWith(
                        color: theme.colorScheme.onSurfaceVariant,
                      ),
                    )
                  else
                    ...paths.take(4).map(
                          (path) => Padding(
                            padding: const EdgeInsets.only(bottom: 6),
                            child: Row(
                              children: [
                                Icon(
                                  path['transport'] == 'cellular'
                                      ? Icons.signal_cellular_alt_rounded
                                      : Icons.wifi_rounded,
                                  size: 18,
                                  color: path['eligible'] == true
                                      ? theme.colorScheme.primary
                                      : theme.colorScheme.onSurfaceVariant,
                                ),
                                const SizedBox(width: 8),
                                Expanded(
                                  child: Text(
                                    _pathLabel(path),
                                    style: theme.textTheme.bodySmall,
                                  ),
                                ),
                              ],
                            ),
                          ),
                        ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }

  Widget _telemetryTextField(
    TextEditingController controller,
    String label, {
    String? hint,
    String? helper,
  }) {
    return TextField(
      controller: controller,
      onChanged: (_) => _markTelemetryDirty(),
      decoration: InputDecoration(
        labelText: label,
        hintText: hint,
        helperText: helper,
        border: const OutlineInputBorder(),
      ),
    );
  }

  Widget _telemetryCard(ThemeData theme) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.only(bottom: 12),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            ListTile(
              leading: const Icon(Icons.insights_rounded),
              title: const Text('OpenTelemetry'),
              subtitle: Text('${_telemetrySummary()} · 重连后生效'),
            ),
            const Divider(height: 0),
            SwitchListTile(
              secondary: const Icon(Icons.power_settings_new_rounded),
              value: _otelEnabled,
              title: const Text('启用 OTEL'),
              subtitle: const Text('把 native 日志、Metrics 和 Spans 下发给 OpenPPP2 引擎'),
              onChanged: (v) => _updateTelemetry(() => _otelEnabled = v),
            ),
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 4, 16, 4),
              child: Column(
                children: [
                  _telemetryTextField(
                    _otelEndpoint,
                    'OTLP Endpoint',
                    hint: 'https://collector.example.com',
                    helper: '留空时只输出到 console / 本地文件',
                  ),
                  const SizedBox(height: 12),
                  DropdownButtonFormField<int>(
                    value: _otelLevel,
                    isExpanded: true,
                    decoration: const InputDecoration(
                      labelText: '日志级别',
                      border: OutlineInputBorder(),
                    ),
                    items: List.generate(
                      4,
                      (i) => DropdownMenuItem<int>(
                        value: i,
                        child: Text('$i · ${_levelLabel(i)}'),
                      ),
                    ),
                    onChanged: (value) {
                      if (value != null) {
                        _updateTelemetry(() => _otelLevel = value);
                      }
                    },
                  ),
                  const SizedBox(height: 4),
                  SwitchListTile(
                    contentPadding: EdgeInsets.zero,
                    value: _otelMetricsEnabled,
                    title: const Text('Metrics'),
                    subtitle: const Text('启用 count / gauge / histogram 指标'),
                    onChanged: (v) =>
                        _updateTelemetry(() => _otelMetricsEnabled = v),
                  ),
                  SwitchListTile(
                    contentPadding: EdgeInsets.zero,
                    value: _otelSpansEnabled,
                    title: const Text('Spans'),
                    subtitle: const Text('启用 native trace span 采集'),
                    onChanged: (v) =>
                        _updateTelemetry(() => _otelSpansEnabled = v),
                  ),
                  ExpansionTile(
                    tilePadding: EdgeInsets.zero,
                    childrenPadding: EdgeInsets.zero,
                    leading: const Icon(Icons.output_rounded),
                    title: const Text('输出选项'),
                    subtitle: Text(
                      'console-log ${_otelConsoleLog ? '开启' : '关闭'} · log-file ${_otelLogFile.text.trim().isEmpty ? '未设置' : '已设置'}',
                      style: theme.textTheme.bodySmall,
                    ),
                    children: [
                      _telemetryTextField(
                        _otelLogFile,
                        'log-file',
                        hint: '/sdcard/Download/openppp2-otel.log',
                        helper: '可选，本地 telemetry 文件路径',
                      ),
                      SwitchListTile(
                        contentPadding: EdgeInsets.zero,
                        value: _otelConsoleLog,
                        title: const Text('console-log'),
                        onChanged: (v) =>
                            _updateTelemetry(() => _otelConsoleLog = v),
                      ),
                      SwitchListTile(
                        contentPadding: EdgeInsets.zero,
                        value: _otelConsoleMetric,
                        title: const Text('console-metric'),
                        onChanged: (v) =>
                            _updateTelemetry(() => _otelConsoleMetric = v),
                      ),
                      SwitchListTile(
                        contentPadding: EdgeInsets.zero,
                        value: _otelConsoleSpan,
                        title: const Text('console-span'),
                        onChanged: (v) =>
                            _updateTelemetry(() => _otelConsoleSpan = v),
                      ),
                    ],
                  ),
                  const SizedBox(height: 10),
                  Row(
                    children: [
                      OutlinedButton.icon(
                        onPressed: _resetTelemetry,
                        icon: const Icon(Icons.restore_rounded),
                        label: const Text('默认'),
                      ),
                      const Spacer(),
                      FilledButton.icon(
                        onPressed: _otelDirty ? _saveTelemetry : null,
                        icon: const Icon(Icons.save_rounded),
                        label: Text(_otelDirty ? '保存' : '已保存'),
                      ),
                    ],
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }

  @override
  void dispose() {
    _pollTimer?.cancel();
    _pathPollTimer?.cancel();
    _stateSub?.cancel();
    _pathSub?.cancel();
    _otelEndpoint.dispose();
    _otelLogFile.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Scaffold(
      appBar: AppBar(
        title: const Text('设置'),
        centerTitle: true,
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : ListView(
              padding: const EdgeInsets.all(16),
              children: [
                Card(
                  child: Column(
                    children: [
                      ListTile(
                        leading: const Icon(Icons.info_outline_rounded),
                        title: const Text('OPENPPP2'),
                        subtitle: const Text('Android Client · v1.0.0'),
                      ),
                      const Divider(height: 0),
                      _ThemeModeTile(),
                      const Divider(height: 0),
                      SwitchListTile(
                        secondary: const Icon(Icons.bug_report_outlined),
                        value: _debugPanelEnabled,
                        title: const Text('显示调试面板'),
                        subtitle: const Text('在主页和此页显示日志/状态/停止按钮'),
                        onChanged: _setDebugPanel,
                      ),
                    ],
                  ),
                ),
                const SizedBox(height: 12),
                _telemetryCard(theme),
                const SizedBox(height: 12),
                _pathAwarenessCard(theme),
                const SizedBox(height: 12),
                if (_debugPanelEnabled)
                  DebugPanel(
                    stateText: _stateText(),
                    logPath: _logPath,
                    logText: _debugLog,
                    onRefresh: _refresh,
                    onCopy: _copyLog,
                    onClear: _clearLog,
                    onStop: _stopVpn,
                  ),
                const SizedBox(height: 16),
                Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 4),
                  child: Text(
                    '服务器请在「服务器」页面中编辑，运行参数请在「参数」页面中维护。',
                    style: theme.textTheme.bodySmall?.copyWith(
                      color: theme.colorScheme.onSurfaceVariant,
                    ),
                  ),
                ),
              ],
            ),
    );
  }
}

class _ThemeModeTile extends StatelessWidget {
  @override
  Widget build(BuildContext context) {
    return ValueListenableBuilder<ThemeMode>(
      valueListenable: ThemeController.instance.mode,
      builder: (context, mode, _) {
        return ListTile(
          leading: Icon(_iconFor(mode)),
          title: const Text('主题'),
          subtitle: Text(_labelFor(mode)),
          trailing: SegmentedButton<ThemeMode>(
            showSelectedIcon: false,
            style: const ButtonStyle(
              visualDensity: VisualDensity.compact,
              tapTargetSize: MaterialTapTargetSize.shrinkWrap,
            ),
            segments: const [
              ButtonSegment(
                value: ThemeMode.system,
                icon: Icon(Icons.brightness_auto_rounded, size: 18),
                tooltip: '跟随系统',
              ),
              ButtonSegment(
                value: ThemeMode.light,
                icon: Icon(Icons.light_mode_rounded, size: 18),
                tooltip: '浅色',
              ),
              ButtonSegment(
                value: ThemeMode.dark,
                icon: Icon(Icons.dark_mode_rounded, size: 18),
                tooltip: '深色',
              ),
            ],
            selected: {mode},
            onSelectionChanged: (s) {
              if (s.isNotEmpty) ThemeController.instance.set(s.first);
            },
          ),
        );
      },
    );
  }

  IconData _iconFor(ThemeMode m) {
    switch (m) {
      case ThemeMode.light:
        return Icons.light_mode_rounded;
      case ThemeMode.dark:
        return Icons.dark_mode_rounded;
      case ThemeMode.system:
        return Icons.brightness_auto_rounded;
    }
  }

  String _labelFor(ThemeMode m) {
    switch (m) {
      case ThemeMode.light:
        return '浅色模式';
      case ThemeMode.dark:
        return '深色模式';
      case ThemeMode.system:
        return '跟随系统';
    }
  }
}
