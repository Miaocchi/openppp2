import 'dart:async';
import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import '../models/telemetry_settings.dart';
import '../services/otlp_exporter.dart';
import '../services/telemetry_identity.dart';
import '../services/telemetry_settings_store.dart';
import '../widgets/app_section_card.dart';
import '../widgets/unsaved_changes_guard.dart';

class TelemetrySettingsPage extends StatefulWidget {
  const TelemetrySettingsPage({super.key});

  @override
  State<TelemetrySettingsPage> createState() => _TelemetrySettingsPageState();
}

class _TelemetrySettingsPageState extends State<TelemetrySettingsPage> {
  final _store = TelemetrySettingsStore();
  final _endpointController = TextEditingController();

  TelemetrySettings _settings = const TelemetrySettings();
  TelemetrySettings _saved = const TelemetrySettings();
  String _machineId = '';
  String _deviceLabel = '';
  bool _loading = true;
  bool _uploading = false;

  @override
  void initState() {
    super.initState();
    unawaited(_load());
  }

  Future<void> _load() async {
    await TelemetryIdentity.installIfNeeded();
    final settings = await _store.settings();
    final id = await TelemetryIdentity.machineId();
    final attrs = await TelemetryIdentity.resourceAttributes();
    final model = attrs['device.model'] ?? 'unknown';
    final osName = attrs['os.name'] ?? 'Android';
    final osVersion = attrs['os.version'] ?? '';
    if (!mounted) return;
    setState(() {
      _settings = settings;
      _saved = settings;
      _machineId = id;
      _deviceLabel = 'device: $model / $osName $osVersion';
      _loading = false;
      _endpointController.text = settings.customEndpoint;
    });
  }

  TelemetrySettings _applyForm() {
    return _settings.copyWith(
      customEndpoint: _endpointController.text.trim(),
    );
  }

  Future<void> _save() async {
    final next = _applyForm();
    if (next.uploadEnabled &&
        next.destination == TelemetryDestination.custom &&
        next.customEndpoint.trim().isEmpty) {
      _showMessage('请填写 OTLP HTTP Endpoint');
      return;
    }
    if (next.uploadEnabled && TelemetrySettings.logsUrl(next.effectiveEndpoint) == null) {
      _showMessage('请填写包含 http:// 或 https:// 和主机名的接收地址');
      return;
    }
    await _store.save(next);
    if (!mounted) return;
    setState(() => _saved = next);
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('遥测设置已保存')),
    );
  }

  Future<void> _copyMachineId() async {
    await Clipboard.setData(ClipboardData(text: _machineId));
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      const SnackBar(content: Text('machine.id 已复制')),
    );
  }

  Future<void> _uploadTestLog() async {
    final next = _applyForm();
    if (!next.canUpload || !next.includeCrashReports) return;
    final destination = TelemetrySettings.logsUrl(next.effectiveEndpoint);
    if (destination == null) { _showMessage('请先填写有效的 HTTP(S) 接收地址'); return; }
    final confirmed = await showDialog<bool>(context: context, builder: (ctx) => AlertDialog(
      scrollable: true, title: const Text('立即发送测试日志？'),
      content: Text('将使用当前填写的地址发送一条测试记录，附带设备标识、型号和系统信息。不会上传整个运行日志，也不会保存设置。\n\n接收地址：$destination'),
      actions: [TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('取消')),
        FilledButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('发送'))],
    ));
    if (!mounted || confirmed != true) return;

    setState(() => _uploading = true);
    try {
      await OtlpExporter.exportLogs(
        settings: next,
        records: [
          OtlpLogRecord(
            timeUnixNano: DateTime.now().microsecondsSinceEpoch * 1000,
            severityText: 'INFO',
            body: 'openppp2 android telemetry test upload',
            attributes: {
              'openppp2.component': const OtlpValue.string('flutter_app'),
              'openppp2.event': const OtlpValue.string('manual_test'),
            },
          ),
        ],
      );
      if (!mounted) return;
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('测试日志已上传')),
      );
    } on OtlpExportException catch (e) {
      _showMessage(e.message);
    } catch (e) {
      _showMessage(e.toString());
    } finally {
      if (mounted) setState(() => _uploading = false);
    }
  }

  void _showMessage(String message) {
    if (!mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text(message)),
    );
  }

  @override
  void dispose() {
    _endpointController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final next = _applyForm();
    final usesCustom = next.destination == TelemetryDestination.custom;
    final nativeEnabled = next.uploadEnabled && next.includeNativeTelemetry;
    final dirty = !mapEquals(next.toMap(), _saved.toMap());

    if (_loading) {
      return const Scaffold(
        body: Center(child: CircularProgressIndicator()),
      );
    }

    return UnsavedChangesGuard(dirty: dirty, child: Scaffold(
      appBar: AppBar(
        title: const Text('遥测'),
        centerTitle: true,
        actions: [
          TextButton(
            onPressed: dirty ? _save : null,
            child: Text(dirty ? '保存更改' : '已保存'),
          ),
        ],
      ),
      body: ListView(
        padding: const EdgeInsets.all(16),
        children: [
          const AppSectionHeader('隐私与上传'),
          Card(
            child: Column(
              children: [
                SwitchListTile(
                  secondary: const Icon(Icons.cloud_upload_outlined),
                  title: const Text('启用遥测上传'),
                  subtitle: const Text('默认关闭。保存后，允许将所选诊断数据及设备标识发送给下方接收方。'),
                  value: next.uploadEnabled,
                  onChanged: (v) => setState(
                    () => _settings = _settings.copyWith(uploadEnabled: v),
                  ),
                ),
                if (next.uploadEnabled) ...[
                const Divider(height: 0),
                Padding(
                  padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
                  child: SegmentedButton<TelemetryDestination>(
                    segments: TelemetryDestination.values
                        .map(
                          (d) => ButtonSegment(
                            value: d,
                            label: Text(d.label),
                          ),
                        )
                        .toList(),
                    selected: {next.destination},
                    onSelectionChanged: (s) => setState(
                      () => _settings = _settings.copyWith(
                        destination: s.first,
                      ),
                    ),
                  ),
                ),
                if (!usesCustom) ...[
                  const SizedBox(height: 8),
                  Padding(
                    padding: const EdgeInsets.symmetric(horizontal: 16),
                    child: Text(
                      TelemetrySettings.developerEndpoint,
                      style: theme.textTheme.bodySmall?.copyWith(
                        color: theme.colorScheme.onSurfaceVariant,
                      ),
                    ),
                  ),
                ],
                if (usesCustom) ...[
                  const SizedBox(height: 8),
                  Padding(
                    padding: const EdgeInsets.fromLTRB(16, 0, 16, 12),
                    child: TextField(
                      controller: _endpointController,
                      decoration: const InputDecoration(
                        labelText: 'OTLP HTTP Endpoint',
                        hintText: 'https://collector.example.com',
                        border: OutlineInputBorder(),
                      ),
                      keyboardType: TextInputType.url,
                      onChanged: (_) => setState(() {}),
                    ),
                  ),
                ] else
                  const SizedBox(height: 12),
                ],
              ],
            ),
          ),
          if (next.uploadEnabled) ...[
          const SizedBox(height: 12),
          const AppSectionHeader('数据'),
          Card(
            child: Column(
              children: [
                SwitchListTile(
                  title: const Text('应用日志 / 崩溃'),
                  subtitle: const Text('允许应用侧日志上传；可手动发送单条测试记录'),
                  value: next.includeCrashReports,
                  onChanged: (v) => setState(
                    () => _settings = _settings.copyWith(
                      includeCrashReports: v,
                    ),
                  ),
                ),
                const Divider(height: 0),
                SwitchListTile(
                  title: const Text('Native Telemetry'),
                  subtitle: const Text('VPN 进程内 C++ 引擎遥测'),
                  value: next.includeNativeTelemetry,
                  onChanged: (v) => setState(
                    () => _settings = _settings.copyWith(
                      includeNativeTelemetry: v,
                    ),
                  ),
                ),
              ],
            ),
          ),
          if (nativeEnabled) ...[
          const SizedBox(height: 12),
          Card(child: ExpansionTile(
            title: const Text('引擎遥测详情'),
            subtitle: const Text('下次连接时应用日志级别与跟踪选项'),
            children: [
                Padding(
                  padding: const EdgeInsets.fromLTRB(16, 12, 16, 0),
                  child: Wrap(spacing: 6, children: [
                    for (final entry in const ['Info', 'Verb', 'Debug', 'Trace'].asMap().entries)
                      ChoiceChip(label: Text(entry.value), selected: entry.key == next.nativeLogLevel.clamp(0, 3),
                        onSelected: (_) => setState(() => _settings = _settings.copyWith(nativeLogLevel: entry.key))),
                  ]),
                ),
                ListTile(title: const Text('Metrics（只读）'),
                  subtitle: Text('指标记录：${next.nativeMetricsEnabled ? '已开启' : '已关闭'} · 此客户端不提供开关')),
                const Divider(height: 0),
                SwitchListTile(
                  title: const Text('Spans'),
                  subtitle: const Text('Trace spans'),
                  value: next.nativeSpansEnabled,
                  onChanged: (v) => setState(() => _settings = _settings.copyWith(nativeSpansEnabled: v)),
                ),
                Padding(
                  padding: const EdgeInsets.fromLTRB(16, 0, 16, 12),
                  child: Text(
                    'Native 遥测在 VPN 服务启动时安装 HTTP 传输；连接时写入配置 telemetry 块。',
                    style: theme.textTheme.bodySmall?.copyWith(
                      color: theme.colorScheme.onSurfaceVariant,
                    ),
                  ),
                ),
            ],
          )),
          ],
          if (next.includeCrashReports) ...[
          const SizedBox(height: 16),
          FilledButton(
            onPressed: next.canUpload && next.includeCrashReports && !_uploading
                ? _uploadTestLog
                : null,
            child: Text(_uploading ? '上传中…' : '上传测试日志'),
          ),
          ],
          ],
          const SizedBox(height: 12),
          Card(child: ExpansionTile(
            leading: const Icon(Icons.fingerprint), title: const Text('查看设备标识'),
            subtitle: const Text('标识可用于关联同一设备的记录。展开查看，不会上传。'),
            expandedCrossAxisAlignment: CrossAxisAlignment.start,
            childrenPadding: const EdgeInsets.fromLTRB(16, 0, 16, 12),
            children: [
              if (_machineId.isEmpty) const Text('设备标识不可用') else ...[
                Text('短 ID: ${_machineId.substring(0, _machineId.length < 12 ? _machineId.length : 12)}'),
                SelectableText('machine.id: $_machineId'),
              ],
              Text(_deviceLabel),
              TextButton.icon(onPressed: _machineId.isEmpty ? null : _copyMachineId,
                icon: const Icon(Icons.copy), label: const Text('复制完整标识到剪贴板')),
            ],
          )),
        ],
      ),
    ));
  }
}
