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

class _ServerActionCard extends StatelessWidget {
  final IconData icon;
  final String title;
  final String subtitle;
  final bool highlighted;
  final VoidCallback onTap;

  const _ServerActionCard({
    required this.icon,
    required this.title,
    required this.subtitle,
    required this.onTap,
    this.highlighted = false,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final color = highlighted
        ? theme.colorScheme.primary
        : theme.colorScheme.onSurfaceVariant;
    return Material(
      color: highlighted
          ? theme.colorScheme.primaryContainer.withValues(alpha: 0.46)
          : theme.colorScheme.surface,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(12),
        side: BorderSide(
          color: highlighted
              ? theme.colorScheme.primary.withValues(alpha: 0.42)
              : theme.colorScheme.outlineVariant,
        ),
      ),
      child: InkWell(
        borderRadius: BorderRadius.circular(12),
        onTap: onTap,
        child: Padding(
          padding: const EdgeInsets.all(12),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Icon(icon, color: color, size: 24),
              const SizedBox(height: 10),
              Text(
                title,
                maxLines: 2,
                overflow: TextOverflow.ellipsis,
                style: theme.textTheme.titleSmall?.copyWith(
                  fontWeight: FontWeight.w900,
                ),
              ),
              const SizedBox(height: 3),
              Text(
                subtitle,
                maxLines: 2,
                overflow: TextOverflow.ellipsis,
                style: theme.textTheme.bodySmall?.copyWith(
                  color: theme.colorScheme.onSurfaceVariant,
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _EmptyServersCard extends StatelessWidget {
  final VoidCallback onAdd;
  final VoidCallback onImport;

  const _EmptyServersCard({
    required this.onAdd,
    required this.onImport,
  });

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Container(
      padding: const EdgeInsets.all(18),
      decoration: BoxDecoration(
        color: theme.colorScheme.surface,
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: theme.colorScheme.outlineVariant),
      ),
      child: Column(
        children: [
          Icon(
            Icons.dns_outlined,
            size: 34,
            color: theme.colorScheme.onSurfaceVariant,
          ),
          const SizedBox(height: 10),
          Text(
            '还没有服务器',
            style: theme.textTheme.titleSmall?.copyWith(
              fontWeight: FontWeight.w900,
            ),
          ),
          const SizedBox(height: 4),
          Text(
            '添加单个服务器，或从远程订阅导入一组服务器。',
            textAlign: TextAlign.center,
            style: theme.textTheme.bodySmall?.copyWith(
              color: theme.colorScheme.onSurfaceVariant,
            ),
          ),
          const SizedBox(height: 14),
          Wrap(
            spacing: 8,
            runSpacing: 8,
            alignment: WrapAlignment.center,
            children: [
              FilledButton.icon(
                onPressed: onAdd,
                icon: const Icon(Icons.add_rounded),
                label: const Text('添加单个服务器'),
              ),
              OutlinedButton.icon(
                onPressed: onImport,
                icon: const Icon(Icons.cloud_download_outlined),
                label: const Text('导入订阅'),
              ),
            ],
          ),
        ],
      ),
    );
  }
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
        SnackBar(content: Text('已导入/更新 $count 台服务器')),
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
        title: const Text('删除服务器'),
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
        title: const Text('服务器'),
        centerTitle: true,
        actions: [
          IconButton(
            icon: const Icon(Icons.cloud_download_outlined),
            tooltip: '导入远程订阅',
            onPressed: _importSubscription,
          ),
          IconButton(
            icon: const Icon(Icons.add_rounded),
            tooltip: '添加单个服务器',
            onPressed: _add,
          ),
        ],
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : ListView(
              padding: const EdgeInsets.fromLTRB(16, 16, 16, 24),
              children: [
                Row(
                  children: [
                    Expanded(
                      child: _ServerActionCard(
                        icon: Icons.add_rounded,
                        title: '添加单个服务器',
                        subtitle: '手动填写名称、地址和 JSON',
                        highlighted: true,
                        onTap: _add,
                      ),
                    ),
                    const SizedBox(width: 10),
                    Expanded(
                      child: _ServerActionCard(
                        icon: Icons.cloud_download_outlined,
                        title: '导入远程订阅',
                        subtitle: '从 URL 同步多台服务器',
                        onTap: _importSubscription,
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 18),
                Row(
                  children: [
                    Expanded(
                      child: Text(
                        '可用服务器',
                        style: theme.textTheme.titleSmall?.copyWith(
                          fontWeight: FontWeight.w900,
                        ),
                      ),
                    ),
                    Text(
                      '${_profiles.length} 台',
                      style: theme.textTheme.bodySmall?.copyWith(
                        color: theme.colorScheme.onSurfaceVariant,
                      ),
                    ),
                  ],
                ),
                const SizedBox(height: 8),
                if (_profiles.isEmpty)
                  _EmptyServersCard(onAdd: _add, onImport: _importSubscription)
                else
                  ..._profiles.map((p) {
                    final isActive = p.id == _activeId;
                    final sub = p.subtitle.isNotEmpty
                        ? p.subtitle
                        : (p.serverEndpoint ?? '');
                    return Padding(
                      padding: const EdgeInsets.only(bottom: 8),
                      child: Material(
                        color: theme.colorScheme.surface,
                        shape: RoundedRectangleBorder(
                          borderRadius: BorderRadius.circular(12),
                          side: BorderSide(
                            color: isActive
                                ? theme.colorScheme.primary
                                : theme.colorScheme.outlineVariant,
                            width: isActive ? 1.6 : 1.0,
                          ),
                        ),
                        child: InkWell(
                          borderRadius: BorderRadius.circular(12),
                          onTap: () => _edit(p),
                          child: Padding(
                            padding: const EdgeInsets.fromLTRB(14, 12, 6, 12),
                            child: Row(
                              children: [
                                Container(
                                  width: 40,
                                  height: 40,
                                  decoration: BoxDecoration(
                                    color: theme.colorScheme.primary
                                        .withValues(alpha: 0.1),
                                    borderRadius: BorderRadius.circular(10),
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
                                    crossAxisAlignment:
                                        CrossAxisAlignment.start,
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
                                                fontWeight: FontWeight.w800,
                                              ),
                                            ),
                                          ),
                                          if (isActive) ...[
                                            const SizedBox(width: 6),
                                            Container(
                                              padding:
                                                  const EdgeInsets.symmetric(
                                                horizontal: 7,
                                                vertical: 2,
                                              ),
                                              decoration: BoxDecoration(
                                                color: theme.colorScheme
                                                    .primaryContainer,
                                                borderRadius:
                                                    BorderRadius.circular(999),
                                              ),
                                              child: Text(
                                                '当前',
                                                style: theme
                                                    .textTheme.labelSmall
                                                    ?.copyWith(
                                                  color: theme.colorScheme
                                                      .onPrimaryContainer,
                                                  fontWeight: FontWeight.w800,
                                                ),
                                              ),
                                            ),
                                          ],
                                        ],
                                      ),
                                      if (sub.isNotEmpty) ...[
                                        const SizedBox(height: 3),
                                        Text(
                                          sub,
                                          maxLines: 1,
                                          overflow: TextOverflow.ellipsis,
                                          style:
                                              theme.textTheme.bodySmall?.copyWith(
                                            color: theme
                                                .colorScheme.onSurfaceVariant,
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
                                          leading:
                                              Icon(Icons.check_circle_outline),
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
                      ),
                    );
                  }),
              ],
            ),
    );
  }
}
