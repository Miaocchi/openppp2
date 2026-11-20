import 'package:flutter/material.dart';

/// For editors with an explicit save action. A back gesture must not silently
/// discard a change (notably a request to disable telemetry).
class UnsavedChangesGuard extends StatefulWidget {
  const UnsavedChangesGuard({super.key, required this.dirty, required this.child});

  final bool dirty;
  final Widget child;

  @override
  State<UnsavedChangesGuard> createState() => _UnsavedChangesGuardState();
}

class _UnsavedChangesGuardState extends State<UnsavedChangesGuard> {
  bool _confirming = false;

  @override
  Widget build(BuildContext context) {
    return PopScope(
      canPop: !widget.dirty,
      onPopInvokedWithResult: (didPop, _) async {
        if (didPop || _confirming) return;
        _confirming = true;
        try {
          final discard = await showDialog<bool>(
            context: context,
            builder: (ctx) => AlertDialog(
              title: const Text('更改尚未保存'),
              content: const Text('返回后将保留原设置。要应用更改，请继续编辑并保存。'),
              actions: [
                TextButton(onPressed: () => Navigator.pop(ctx, true), child: const Text('放弃并返回')),
                FilledButton(onPressed: () => Navigator.pop(ctx, false), child: const Text('继续编辑')),
              ],
            ),
          );
          if (discard == true && context.mounted) Navigator.pop(context);
        } finally {
          _confirming = false;
        }
      },
      child: widget.child,
    );
  }
}
