import 'package:flutter/material.dart';
import '../models/config_profile.dart';
import '../services/profile_store.dart';
import '../widgets/profile_ui.dart';
import 'profile_edit_page.dart';

class SelectProfilePage extends StatefulWidget {
  const SelectProfilePage({super.key});

  @override
  State<SelectProfilePage> createState() => _SelectProfilePageState();
}

class _SelectProfilePageState extends State<SelectProfilePage> {
  final _store = ProfileStore();
  List<ConfigProfile> _profiles = const [];
  String? _activeId;
  bool _loading = true;

  @override
  void initState() {
    super.initState();
    _load();
  }

  Future<void> _load() async {
    final profiles = await _store.getProfiles();
    final active = await _store.getActive();
    if (!mounted) return;
    setState(() {
      _profiles = profiles;
      _activeId = active?.id;
      _loading = false;
    });
  }

  Future<void> _select(ConfigProfile p) async {
    await _store.setActive(p.id);
    if (!mounted) return;
    Navigator.of(context).pop(p.id);
  }

  Future<void> _togglePin(ConfigProfile p) async {
    await _store.toggleFavorite(p.id);
    await _load();
  }

  Future<void> _add() async {
    final created = await Navigator.of(context).push<bool>(
      MaterialPageRoute(builder: (_) => const ProfileEditPage()),
    );
    if (created == true) await _load();
  }


  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        title: const Text('选择节点'),
        centerTitle: true,
        actions: [
          IconButton(
            icon: const Icon(Icons.add_rounded),
            tooltip: '新增配置',
            onPressed: _add,
          ),
        ],
      ),
      body: _loading
          ? const Center(child: CircularProgressIndicator())
          : Padding(
              padding: const EdgeInsets.fromLTRB(16, 8, 16, 8),
              child: SearchableProfileList(
                profiles: _profiles, activeId: _activeId,
                onTap: _select, onTogglePin: _togglePin, onCreate: _add,
              ),
            ),
    );
  }
}
