import 'package:flutter/material.dart';
import '../models/config_profile.dart';
import '../services/profile_store.dart';
import '../services/subscription_service.dart';
import 'profile_edit_page.dart';

class ProfilesPage extends StatefulWidget {
  const ProfilesPage({super.key});

  @override
  State<ProfilesPage> createState() => _ProfilesPageState();
}

class _ProfilesPageState extends State<ProfilesPage> {
  final _store = ProfileStore();
  List<ConfigProfile> _profiles = const [];
  String? _activeId;
  bool _loading = true;

  @override
  void initState() {
    super.initState();
    _load();
    _store.changes.listen((_) {
      if (mounted) _load();
    });
  }

  Future<void> _load() async {
    final list = await _store.getProfiles();
    final active = await _store.getActive();
    if (!mounted) return;
    setState(() {
      _profiles = list;
      _activeId = active?.id;
      _loading = false;
    });
  }

  Future<void> _add() async {
    final ok = await Navigator.of(context).push<bool>(
      MaterialPageRoute(builder: (_) => const ProfileEditPage()),
    );
    if (ok == true) await _load();
  }

  Future<String?> _askSubscriptionUrl() async {
    final controller = TextEditingController();
    try {
      return await showDialog<String>(
        context: context,
        builder: (ctx) => AlertDialog(
          title: const Text('导入远程订阅'),
          content: TextField(
            controller: controller,
            keyboardType: TextInputType.url,
            autofocus: true,
            decoration: const InputDecoration(
              labelText: '订阅 URL',
              hintText: 'https://example.com/openppp2.json',
            ),
          ),
          actions: [
            TextButton(
              onPressed: () => Navigator.of(ctx).pop(),
              child: const Text('取消'),
            ),
            FilledButton(
              onPressed: () => Navigator.of(ctx).pop(controller.text.trim()),
              child: const Text('导入'),
            ),
          ],
        ),
      );
    } finally {
      controller.dispose();
    }
  }

  Future<void> _importSubscription() async {
    final url = await _askSubscriptionUrl();
    if (url == null || url.isEmpty) return;

    var progressShown = false;
    if (mounted) {
      progressShown = true;
      showDialog<void>(
        context: context,
        barrierDismissible: false,
        builder: (_) => const Center(child: CircularProgressIndicator()),
      );
    }

    try {
      final subscription = await SubscriptionService().fetch(url);
      final count = await _store.upsertSubscription(
        url: url,
        subscription: subscription,
      );
      if (!mounted) return;
      if (progressShown) Navigator.of(context, rootNavigator: true).pop();
      await _load();
      ScaffoldMessenger.of(context).showSnackBar(
        SnackBar(content: Text('已导入/更新 $count 个节点')),
      );
    } catch (e) {
      if (!mounted) return;
      if (progressShown) Navigator.of(context, rootNavigator: true).pop();
      await showDialog<void>(
        context: context,
        builder: (ctx) => AlertDialog(
          title: const Text('订阅导入失败'),
          content: SelectableText(e.toString()),
          actions: [
            TextButton(
              onPressed: () => Navigator.of(ctx).pop(),
              child: const Text('关闭'),
            ),
          ],
        ),
      );
    }
  }

  Future<void> _edit(ConfigProfile p) async {
    final ok = await Navigator.of(context).push<bool>(
      MaterialPageRoute(builder: (_) => ProfileEditPage(profile: p)),
    );
    if (ok == true) await _load();
  }

  Future<void> _delete(ConfigProfile p) async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('删除配置'),
        content: Text('确定要删除「${p.name}」吗？'),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(ctx).pop(false),
            child: const Text('取消'),
          ),
          FilledButton.tonal(
            onPressed: () => Navigator.of(ctx).pop(true),
            child: const Text('删除'),
          ),
        ],
      ),
    );
    if (ok == true) {
      await _store.remove(p.id);
      await _load();
    }
  }

  Future<void> _setActive(ConfigProfile p) async {
    await _store.setActive(p.id);
    await _load();
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Scaffold(
      appBar: AppBar(
        title: const Text('配置文件'),
        centerTitle: true,
        actions: [
          IconButton(
            icon: const Icon(Icons.cloud_download_outlined),
            tooltip: '导入远程订阅',
            onPressed: _importSubscription,
          ),
          IconButton(
            icon: const Icon(Icons.add_rounded),
            tooltip: '新增配置',
            onPressed: _add,
          ),
        ],
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : ListView.separated(
              padding: const EdgeInsets.all(16),
              itemCount: _profiles.length,
              separatorBuilder: (_, __) => const SizedBox(height: 8),
              itemBuilder: (ctx, i) {
                final p = _profiles[i];
                final isActive = p.id == _activeId;
                final sub = p.subtitle.isNotEmpty
                    ? p.subtitle
                    : (p.serverEndpoint ?? '');
                return Material(
                  color: theme.colorScheme.surface,
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(14),
                    side: BorderSide(
                      color: isActive
                          ? theme.colorScheme.primary
                          : theme.colorScheme.outlineVariant,
                      width: isActive ? 1.6 : 1.0,
                    ),
                  ),
                  child: InkWell(
                    borderRadius: BorderRadius.circular(14),
                    onTap: () => _edit(p),
                    child: Padding(
                      padding: const EdgeInsets.fromLTRB(14, 12, 6, 12),
                      child: Row(
                        children: [
                          Container(
                            width: 36,
                            height: 36,
                            decoration: BoxDecoration(
                              color: theme.colorScheme.primary
                                  .withValues(alpha: 0.1),
                              shape: BoxShape.circle,
                            ),
                            alignment: Alignment.center,
                            child: Text(
                              p.flag.isNotEmpty ? p.flag : '🌐',
                              style: const TextStyle(fontSize: 20),
                            ),
                          ),
                          const SizedBox(width: 12),
                          Expanded(
                            child: Column(
                              crossAxisAlignment: CrossAxisAlignment.start,
                              children: [
                                Row(
                                  children: [
                                    Flexible(
                                      child: Text(
                                        p.name,
                                        maxLines: 1,
                                        overflow: TextOverflow.ellipsis,
                                        style: theme.textTheme.titleSmall
                                            ?.copyWith(
                                          fontWeight: FontWeight.w700,
                                        ),
                                      ),
                                    ),
                                    if (isActive) ...[
                                      const SizedBox(width: 6),
                                      Container(
                                        padding: const EdgeInsets.symmetric(
                                            horizontal: 6, vertical: 1),
                                        decoration: BoxDecoration(
                                          color: theme.colorScheme.primary,
                                          borderRadius:
                                              BorderRadius.circular(8),
                                        ),
                                        child: Text(
                                          'ACTIVE',
                                          style: theme.textTheme.labelSmall
                                              ?.copyWith(
                                            color: theme.colorScheme.onPrimary,
                                            fontWeight: FontWeight.w700,
                                            letterSpacing: 0.6,
                                          ),
                                        ),
                                      ),
                                    ],
                                  ],
                                ),
                                if (sub.isNotEmpty) ...[
                                  const SizedBox(height: 2),
                                  Text(
                                    sub,
                                    maxLines: 1,
                                    overflow: TextOverflow.ellipsis,
                                    style: theme.textTheme.bodySmall?.copyWith(
                                      color: theme.colorScheme.onSurfaceVariant,
                                    ),
                                  ),
                                ],
                              ],
                            ),
                          ),
                          PopupMenuButton<String>(
                            onSelected: (v) {
                              switch (v) {
                                case 'use':
                                  _setActive(p);
                                  break;
                                case 'edit':
                                  _edit(p);
                                  break;
                                case 'delete':
                                  _delete(p);
                                  break;
                              }
                            },
                            itemBuilder: (_) => [
                              if (!isActive)
                                const PopupMenuItem(
                                  value: 'use',
                                  child: ListTile(
                                    leading: Icon(Icons.check_circle_outline),
                                    title: Text('设为当前'),
                                    contentPadding: EdgeInsets.zero,
                                    dense: true,
                                  ),
                                ),
                              const PopupMenuItem(
                                value: 'edit',
                                child: ListTile(
                                  leading: Icon(Icons.edit_outlined),
                                  title: Text('编辑'),
                                  contentPadding: EdgeInsets.zero,
                                  dense: true,
                                ),
                              ),
                              const PopupMenuItem(
                                value: 'delete',
                                child: ListTile(
                                  leading: Icon(Icons.delete_outline),
                                  title: Text('删除'),
                                  contentPadding: EdgeInsets.zero,
                                  dense: true,
                                ),
                              ),
                            ],
                          ),
                        ],
                      ),
                    ),
                  ),
                );
              },
            ),
      floatingActionButton: FloatingActionButton.extended(
        onPressed: _add,
        icon: const Icon(Icons.add_rounded),
        label: const Text('新增配置'),
      ),
    );
  }
}
