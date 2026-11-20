import 'dart:convert';

import 'package:flutter/material.dart';

import '../models/app_scope.dart';
import '../models/config_profile.dart';
import '../services/profile_store.dart';
import '../vpn_service.dart';

/// Per-app proxy editor.
///
/// Reads/writes `options.perAppProxyEnabled`, `options.perAppProxyMode`
/// (`'allow'` | `'deny'`), and `options.perAppProxyApps` (list of package
/// names) on the *active* profile. Review also includes saved packages that
/// aren't visible in the current installed-app list.
class PerAppProxyPage extends StatefulWidget {
  const PerAppProxyPage({super.key});

  @override
  State<PerAppProxyPage> createState() => _PerAppProxyPageState();
}

class _AppEntry {
  final String label;
  final bool system;
  const _AppEntry({required this.label, required this.system});
}

class _PerAppProxyPageState extends State<PerAppProxyPage> {
  final _store = ProfileStore();
  final _vpn = VpnService();
  final _searchController = TextEditingController();
  final _iconCache = <String, String>{}; // package -> base64 PNG

  ConfigProfile? _profile;
  Map<String, _AppEntry> _allApps = const {};
  List<String>? _matchingInstalled;
  Set<String> _selected = <String>{};
  bool _enabled = false;
  String _mode = 'allow';
  bool _includeSystem = false;
  bool _selectedOnly = false;
  String _query = '';
  bool _loading = true;
  bool _dirty = false;
  bool _leaving = false;

  @override
  void initState() {
    super.initState();
    _searchController.addListener(() {
      final query = _searchController.text.trim().toLowerCase();
      if (query == _query) return;
      setState(() {
        _query = query;
        _matchingInstalled = null;
      });
    });
    _load();
  }

  Future<void> _load() async {
    final active = await _store.getActive();
    if (active == null) {
      if (!mounted) return;
      setState(() {
        _profile = null;
        _loading = false;
      });
      return;
    }
    final options = await _store.getProfileOptions(active.id);
    final enabled = options['perAppProxyEnabled'] == true;
    final mode = (options['perAppProxyMode'] ?? 'allow').toString();
    final raw = options['perAppProxyApps'];
    final selected = <String>{};
    if (raw is List) {
      for (final v in raw) {
        if (v is String && v.trim().isNotEmpty) selected.add(v);
      }
    }
    final apps = await _readApps();
    if (!mounted) return;
    setState(() {
      _profile = active;
      _enabled = enabled;
      _mode = mode == 'deny' ? 'deny' : 'allow';
      _selected = selected;
      _allApps = apps;
      _matchingInstalled = null;
      _loading = false;
    });
  }

  Future<Map<String, _AppEntry>> _readApps() async {
    final apps = await _vpn.getInstalledApps(includeSystem: _includeSystem);
    final result = <String, _AppEntry>{};
    for (final app in apps) {
      final package = (app['package'] ?? '').toString();
      if (package.isEmpty) continue;
      result[package] = _AppEntry(
        label: (app['label'] ?? package).toString(),
        system: app['system'] == true,
      );
    }
    return result;
  }

  Future<void> _reloadAppList() async {
    setState(() => _loading = true);
    final apps = await _readApps();
    if (!mounted) return;
    setState(() {
      _allApps = apps;
      _matchingInstalled = null;
      _loading = false;
    });
  }

  List<String> get _filtered {
    List<String> matching(Iterable<String> packages) {
      if (_query.isEmpty) return packages.toList(growable: false);
      return packages.where((package) =>
          package.toLowerCase().contains(_query) ||
          (_allApps[package]?.label.toLowerCase().contains(_query) ?? false)
      ).toList(growable: false);
    }

    // Keep the installed list in its loaded order, independent of checkboxes.
    if (_selectedOnly) return matching(_selected);
    return _matchingInstalled ??= matching(_allApps.keys);
  }

  Future<String?> _iconFor(String package) async {
    final cached = _iconCache[package];
    if (cached != null) return cached;
    final v = await _vpn.getAppIcon(package);
    if (v != null) _iconCache[package] = v;
    return v;
  }

  Future<void> _save({bool showSnack = true}) async {
    final p = _profile;
    if (p == null) return;
    final options = await _store.getProfileOptions(p.id);
    options['perAppProxyEnabled'] = _enabled;
    options['perAppProxyMode'] = _mode;
    options['perAppProxyApps'] = _selected.toList()..sort();
    await _store.setProfileOptions(p.id, options);
    if (!mounted) return;
    setState(() => _dirty = false);
    if (showSnack) {
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('已保存到「${p.name}」（${_selected.length} 个应用）')),
      );
    }
  }

  Future<bool> _confirmDiscard() async {
    if (!_dirty) return true;
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('未保存'),
        content: const Text('返回前是否保存修改？'),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(ctx).pop(null),
            child: const Text('取消'),
          ),
          TextButton(
            onPressed: () => Navigator.of(ctx).pop(false),
            child: const Text('放弃'),
          ),
          FilledButton.tonal(
            onPressed: () => Navigator.of(ctx).pop(true),
            child: const Text('保存'),
          ),
        ],
      ),
    );
    if (ok == null) return false;
    if (ok) await _save(showSnack: false);
    return true;
  }

  Future<void> _leave({required bool save}) async {
    if (_leaving) return;
    setState(() => _leaving = true);
    try {
      if (save) {
        if (_dirty) await _save();
      } else if (!await _confirmDiscard()) {
        return;
      }
      if (mounted && ModalRoute.of(context)?.isCurrent == true) Navigator.pop(context);
    } finally {
      if (mounted) setState(() => _leaving = false);
    }
  }

  @override
  void dispose() {
    _searchController.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final filtered = _filtered;
    final allFilteredSelected =
        filtered.isNotEmpty && (_selectedOnly || filtered.every(_selected.contains));
    return PopScope(
      canPop: !_dirty && !_leaving,
      onPopInvokedWithResult: (didPop, _) {
        if (!didPop) _leave(save: false);
      },
      child: Scaffold(
        appBar: AppBar(
          title: const Text('分应用代理'),
          centerTitle: true,
          actions: [
            IconButton(
              tooltip: allFilteredSelected ? '取消选中筛选结果' : '全选筛选结果',
              icon: Icon(allFilteredSelected
                  ? Icons.deselect_rounded
                  : Icons.select_all_rounded),
              onPressed: !_loading && !_leaving && (_enabled || _selectedOnly) && filtered.isNotEmpty
                  ? () {
                      setState(() {
                        if (allFilteredSelected) {
                          _selected.removeAll(filtered);
                        } else {
                          _selected.addAll(filtered);
                        }
                        _dirty = true;
                      });
                    }
                  : null,
            ),
            TextButton(
              onPressed: _loading || _leaving ? null : () => _leave(save: true),
              child: Text(_dirty ? '保存并返回' : '完成'),
            ),
          ],
        ),
        body: _loading
            ? const Center(child: CircularProgressIndicator())
            : _profile == null
                ? Center(
                    child: Padding(
                      padding: const EdgeInsets.all(24),
                      child: Text(
                        '请先在「配置文件」页中创建并选择一个配置',
                        textAlign: TextAlign.center,
                        style: theme.textTheme.bodyMedium,
                      ),
                    ),
                  )
                : IgnorePointer(ignoring: _leaving, child: CustomScrollView(
                    keyboardDismissBehavior: ScrollViewKeyboardDismissBehavior.onDrag,
                    slivers: [
                      SliverToBoxAdapter(child: _Header(
                        profileName: _profile!.name,
                        enabled: _enabled,
                        mode: _mode,
                        selectedCount: _selected.length,
                        totalCount: _allApps.length,
                        includeSystem: _includeSystem,
                        selectedOnly: _selectedOnly,
                        onSelectedOnlyChanged: (v) => setState(() => _selectedOnly = v),
                        searchController: _searchController,
                        onEnabledChanged: (v) {
                          setState(() {
                            _enabled = v;
                            _dirty = true;
                          });
                        },
                        onModeChanged: (m) {
                          setState(() {
                            _mode = m;
                            _dirty = true;
                          });
                        },
                        onIncludeSystemChanged: (v) async {
                          setState(() => _includeSystem = v);
                          await _reloadAppList();
                        },
                      )),
                      const SliverToBoxAdapter(child: Divider(height: 1)),
                      if (filtered.isEmpty)
                        SliverToBoxAdapter(
                          child: Padding(
                            padding: const EdgeInsets.all(24),
                            child: Column(children: [
                              Text(_query.isNotEmpty ? '无匹配的应用' :
                                _selectedOnly ? '尚未选择应用' : '未加载到应用'),
                              TextButton(
                                onPressed: _query.isNotEmpty ? _searchController.clear : _selectedOnly
                                  ? () => setState(() => _selectedOnly = false) : _reloadAppList,
                                child: Text(_query.isNotEmpty ? '清除搜索' : _selectedOnly ? '浏览应用列表' : '重新加载'),
                              ),
                            ]),
                          ),
                        )
                      else
                        SliverList.builder(
                          itemCount: filtered.length,
                          itemBuilder: (ctx, i) {
                            final package = filtered[i];
                            final app = _allApps[package];
                            void toggle(bool checked) => setState(() {
                              if (checked) {
                                _selected.add(package);
                              } else {
                                _selected.remove(package);
                              }
                              _dirty = true;
                            });
                            if (app == null) {
                              return CheckboxListTile(
                                key: ValueKey(package),
                                title: Text(package),
                                subtitle: const Text('不在当前应用列表中'),
                                value: true,
                                onChanged: (_) => toggle(false),
                              );
                            }
                            return _AppTile(
                              key: ValueKey(package),
                              label: app.label,
                              package: package,
                              system: app.system,
                              checked: _selected.contains(package),
                              enabled: _enabled || _selectedOnly,
                              iconLoader: () => _iconFor(package),
                              onToggle: toggle,
                            );
                          },
                        ),
                    ],
                  )),
      ),
    );
  }
}

class _Header extends StatelessWidget {
  final String profileName;
  final bool enabled;
  final String mode;
  final int selectedCount;
  final int totalCount;
  final bool includeSystem;
  final bool selectedOnly;
  final ValueChanged<bool> onSelectedOnlyChanged;
  final TextEditingController searchController;
  final ValueChanged<bool> onEnabledChanged;
  final ValueChanged<String> onModeChanged;
  final ValueChanged<bool> onIncludeSystemChanged;

  const _Header({
    required this.profileName,
    required this.enabled,
    required this.mode,
    required this.selectedCount,
    required this.totalCount,
    required this.includeSystem,
    required this.selectedOnly,
    required this.onSelectedOnlyChanged,
    required this.searchController,
    required this.onEnabledChanged,
    required this.onModeChanged,
    required this.onIncludeSystemChanged,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 12, 16, 8),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Row(
            children: [
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      '当前编辑: $profileName',
                      style: theme.textTheme.titleSmall
                          ?.copyWith(fontWeight: FontWeight.w800),
                    ),
                    Text(
                      '已选 $selectedCount 个应用 · 已加载 $totalCount 个',
                      style: theme.textTheme.bodySmall?.copyWith(
                        color: theme.colorScheme.onSurfaceVariant,
                      ),
                    ),
                  ],
                ),
              ),
              Semantics(
                label: '启用分应用代理',
                child: Switch(value: enabled, onChanged: onEnabledChanged),
              ),
            ],
          ),
          const SizedBox(height: 8),
          Wrap(
            spacing: 8,
            children: [
              for (final value in const ['allow', 'deny'])
                ChoiceChip(
                  label: Text(value == 'allow' ? '仅允许选中' : '排除选中'),
                  selected: mode == value,
                  onSelected: enabled ? (_) => onModeChanged(value) : null,
                ),
            ],
          ),
          Text(appScopeSummary(enabled: enabled, mode: mode, count: selectedCount)),
          Text('保存后用于下次连接。实际进入隧道的流量还取决于路由规则。', style: theme.textTheme.bodySmall),
          const SizedBox(height: 12),
          TextField(
            controller: searchController,
            textInputAction: TextInputAction.search,
            decoration: InputDecoration(
              isDense: true,
              prefixIcon: const Icon(Icons.search_rounded, size: 20),
              suffixIcon: searchController.text.isEmpty ? null : IconButton(
                tooltip: '清除搜索', icon: const Icon(Icons.close), onPressed: searchController.clear),
              hintText: '搜索应用名 / 包名',
              border: OutlineInputBorder(borderRadius: BorderRadius.circular(10)),
            ),
          ),
          Wrap(
            spacing: 8,
            children: [
              FilterChip(
                selected: selectedOnly,
                label: const Text('仅看已选'),
                onSelected: onSelectedOnlyChanged,
              ),
              FilterChip(
                selected: includeSystem,
                label: const Text('含系统'),
                onSelected: onIncludeSystemChanged,
              ),
            ],
          ),
        ],
      ),
    );
  }
}

class _AppTile extends StatefulWidget {
  final String label;
  final String package;
  final bool system;
  final bool checked;
  final bool enabled;
  final Future<String?> Function() iconLoader;
  final ValueChanged<bool> onToggle;

  const _AppTile({
    super.key,
    required this.label,
    required this.package,
    required this.system,
    required this.checked,
    required this.enabled,
    required this.iconLoader,
    required this.onToggle,
  });

  @override
  State<_AppTile> createState() => _AppTileState();
}

class _AppTileState extends State<_AppTile> {
  String? _iconBase64;
  bool _loadingIcon = false;

  @override
  void initState() {
    super.initState();
    _loadIcon();
  }

  Future<void> _loadIcon() async {
    if (_loadingIcon) return;
    _loadingIcon = true;
    final v = await widget.iconLoader();
    if (!mounted) return;
    setState(() => _iconBase64 = v);
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    Widget leading;
    if (_iconBase64 != null && _iconBase64!.isNotEmpty) {
      leading = ClipRRect(
        borderRadius: BorderRadius.circular(8),
        child: Image.memory(
          base64Decode(_iconBase64!),
          width: 36,
          height: 36,
          fit: BoxFit.cover,
          gaplessPlayback: true,
        ),
      );
    } else {
      leading = Container(
        width: 36,
        height: 36,
        decoration: BoxDecoration(
          color: theme.colorScheme.primary.withValues(alpha: 0.15),
          borderRadius: BorderRadius.circular(8),
        ),
        alignment: Alignment.center,
        child: Text(
          widget.label.isNotEmpty ? widget.label.characters.first : '?',
          style: TextStyle(
            color: theme.colorScheme.primary,
            fontWeight: FontWeight.w800,
          ),
        ),
      );
    }
    return Opacity(
      opacity: widget.enabled ? 1 : 0.55,
      child: CheckboxListTile(
        value: widget.checked,
        onChanged: widget.enabled
            ? (v) => widget.onToggle(v ?? false)
            : null,
        controlAffinity: ListTileControlAffinity.trailing,
        secondary: leading,
        title: Text(
          widget.label,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
        ),
        subtitle: Text(
          widget.system ? '${widget.package} · 系统' : widget.package,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: theme.textTheme.bodySmall?.copyWith(
            color: theme.colorScheme.onSurfaceVariant,
          ),
        ),
        dense: true,
      ),
    );
  }
}
