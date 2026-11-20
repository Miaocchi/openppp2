import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import '../runtime/runtime_controls.dart';
import '../runtime/runtime_store.dart';
import '../services/profile_store.dart';
import '../services/telemetry_settings_store.dart';
import '../services/theme_controller.dart';
import '../vpn_service.dart';
import '../widgets/app_section_card.dart';
import '../widgets/debug_panel.dart';
import 'options_advanced_page.dart';
import 'telemetry_settings_page.dart';

class SettingsPage extends StatefulWidget {
  const SettingsPage({super.key});

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  final _store = ProfileStore();
  final _vpnService = VpnService();

  bool _debugPanelEnabled = false;
  bool _loading = true;
  bool _telemetryUploadEnabled = false;
  String _debugLog = '';
  String _logPath = '';
  Timer? _pollTimer;
  late final RuntimeStore _runtimeStore;

  @override
  void initState() {
    super.initState();
    _vpnService.init();
    _runtimeStore = _vpnService.runtimeStore;
    _runtimeStore.addListener(_runtimeChanged);
    _load();
  }

  void _runtimeChanged() {
    if (!mounted) return;
    setState(() {});
  }

  Future<void> _load() async {
    final enabled = await _store.getDebugPanelEnabled();
    final telemetry = await TelemetrySettingsStore().settings();
    if (!mounted) return;
    setState(() {
      _debugPanelEnabled = enabled;
      _telemetryUploadEnabled = telemetry.uploadEnabled;
      _loading = false;
    });
    if (enabled) {
      _startPoll();
      unawaited(_refresh());
    }
  }

  void _startPoll() {
    _pollTimer?.cancel();
    _pollTimer = Timer.periodic(const Duration(seconds: 2), (_) {
      unawaited(_refresh());
    });
  }

  Future<void> _refresh() async {
    final log = await _vpnService.readLog();
    final path = await _vpnService.getLogPath();
    if (!mounted || !_debugPanelEnabled) return;
    setState(() {
      _debugLog = log;
      _logPath = path;
    });
  }

  Future<void> _setDebugPanel(bool v) async {
    setState(() => _debugPanelEnabled = v);
    await _store.setDebugPanelEnabled(v);
    if (!mounted || _debugPanelEnabled != v) return;
    if (v) {
      _startPoll();
      unawaited(_refresh());
    } else {
      _pollTimer?.cancel();
      _pollTimer = null;
    }
  }

  Future<bool> _confirmAction(String title, String detail, String action) async {
    return await showDialog<bool>(context: context, builder: (ctx) => AlertDialog(
      title: Text(title), content: Text(detail), actions: [
        TextButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('取消')),
        FilledButton(onPressed: () => Navigator.pop(ctx, true), child: Text(action)),
      ],
    )) == true;
  }

  Future<void> _refreshStatus() async {
    final raw = await _vpnService.getRuntimeSnapshot();
    if (!mounted) return;
    _vpnService.applyRuntimeSnapshotPoll(raw, connecting: _vpnService.connecting);
    ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('VPN 状态：${_stateText()}')));
  }

  Future<void> _copyLog() async {
    if (!await _confirmAction('复制原始日志？', '剪贴板将包含完整日志，而不仅是面板中的摘要。分享前请检查服务器地址、路径和其他设备信息。', '复制')) return;
    await Clipboard.setData(ClipboardData(
      text: _debugLog.isEmpty ? '(暂无日志)' : _debugLog,
    ));
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('日志已复制')),
    );
  }

  Future<void> _clearLog() async {
    if (!await _confirmAction('清空运行日志？', '本机现有的诊断记录将被删除，无法撤销。不会删除配置。', '清空')) return;
    await _vpnService.clearLog();
    await _refresh();
  }

  Future<void> _stopVpn() async {
    if (!await _confirmAction('停止 VPN？', '当前连接将断开，应用流量将不再通过此隧道。', '断开')) return;
    await _vpnService.disconnect();
    await _refresh();
  }

  Future<void> _confirmResetProfiles() async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('清空配置文件'),
        content: const Text('所有本地配置、已导入的订阅节点及其历史版本都将被删除，只留下一个默认配置。此操作无法撤销。'),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(ctx).pop(false),
            child: const Text('取消'),
          ),
          FilledButton(
            style: FilledButton.styleFrom(
              backgroundColor: Theme.of(ctx).colorScheme.error,
            ),
            onPressed: () => Navigator.of(ctx).pop(true),
            child: const Text('清空'),
          ),
        ],
      ),
    );
    if (ok != true) return;
    await _store.resetAll();
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('配置文件已重置')),
    );
  }

  String _stateText() => controlsFor(_runtimeStore.state.phase).statusLabel;

  @override
  void dispose() {
    _pollTimer?.cancel();
    _runtimeStore.removeListener(_runtimeChanged);
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    if (_loading) {
      return const Scaffold(
        body: Center(child: CircularProgressIndicator()),
      );
    }

    return Scaffold(
      appBar: AppBar(
        title: const Text('设置'),
        centerTitle: true,
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          const AppSectionHeader('应用'),
          Card(
            child: Column(
              children: [
                const ListTile(
                  leading: Icon(Icons.info_outline_rounded),
                  title: Text('OPENPPP2'),
                  subtitle: Text('Android Client'),
                ),
                const Divider(height: 0),
                _ThemeModeTile(),
              ],
            ),
          ),
          const SizedBox(height: 12),
          const AppSectionHeader('隐私与诊断'),
          Card(
            child: Column(
              children: [
                SwitchListTile(
                  secondary: const Icon(Icons.bug_report_outlined),
                  value: _debugPanelEnabled,
                  title: const Text('调试面板'),
                  subtitle: const Text('在主页和设置页显示本机日志与诊断信息。此开关不会启用遥测上传。'),
                  onChanged: _setDebugPanel,
                ),
                const Divider(height: 0),
                ListTile(
                  leading: const Icon(Icons.analytics_outlined),
                  title: const Text('遥测'),
                  subtitle: Text(
                    _telemetryUploadEnabled ? '已允许上传 · 管理接收方和数据类型' : '上传已关闭 · 查看数据与设备标识',
                  ),
                  trailing: const Icon(Icons.chevron_right_rounded),
                  onTap: () async {
                    await Navigator.of(context).push(
                      MaterialPageRoute(
                        builder: (_) => const TelemetrySettingsPage(),
                      ),
                    );
                    final telemetry = await TelemetrySettingsStore().settings();
                    if (!mounted) return;
                    setState(
                      () => _telemetryUploadEnabled = telemetry.uploadEnabled,
                    );
                  },
                ),
              ],
            ),
          ),
          const SizedBox(height: 12),
          Card(child: ExpansionTile(
            title: const Text('连接工具'),
            subtitle: const Text('检查 VPN 状态或编辑当前配置的高级参数'),
            children: [
                ListTile(
                  leading: const Icon(Icons.refresh_rounded),
                  title: const Text('刷新 VPN 状态'),
                  subtitle: Text(_stateText()),
                  onTap: _refreshStatus,
                ),
                const Divider(height: 0),
                ListTile(
                  leading: const Icon(Icons.tune_rounded),
                  title: const Text('高级启动参数'),
                  subtitle: const Text('分应用代理、Geo 规则生成器等 Android 扩展项'),
                  trailing: const Icon(Icons.chevron_right_rounded),
                  onTap: () {
                    Navigator.of(context).push(
                      MaterialPageRoute(
                        builder: (_) => const OptionsAdvancedPage(),
                      ),
                    );
                  },
                ),
            ],
          )),
          if (_debugPanelEnabled) ...[
            const SizedBox(height: 12),
            DebugPanel(
              stateText: _stateText(),
              logPath: _logPath,
              logText: _debugLog,
              onRefresh: _refresh,
              onCopy: _copyLog,
              onClear: _clearLog,
              onStop: _stopVpn,
              runtimeSnapshot: _vpnService.runtimeStore.state,
            ),
          ],
          const SizedBox(height: 12),
          const AppSectionHeader('危险操作'),
          Card(
            child: ListTile(
              leading: Icon(Icons.delete_forever_outlined,
                  color: theme.colorScheme.error),
              title: Text(
                '清空配置文件',
                style: TextStyle(color: theme.colorScheme.error),
              ),
              subtitle: const Text('删除所有本地节点和历史版本；此操作不可撤销'),
              onTap: _confirmResetProfiles,
            ),
          ),
          const SizedBox(height: 16),
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 4),
            child: Text(
              '常规启动参数请在「启动参数」页编辑；配置文件请在「配置文件」页管理。',
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
