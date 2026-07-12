import 'package:flutter_test/flutter_test.dart';
import 'package:openppp2_mobile/runtime/runtime_controls.dart';
import 'package:openppp2_mobile/runtime/runtime_snapshot.dart';

void main() {
  test('idle enables start and profile editing', () {
    final controls = controlsFor(RuntimePhase.idle);
    expect(controls.action, RuntimeConnectionAction.start);
    expect(controls.buttonEnabled, isTrue);
    expect(controls.buttonLabel, '连接');
    expect(controls.configEditable, isTrue);
  });

  test('connected and reconnecting enable stop', () {
    for (final phase in [RuntimePhase.connected, RuntimePhase.reconnecting]) {
      final controls = controlsFor(phase);
      expect(controls.action, RuntimeConnectionAction.stop);
      expect(controls.buttonEnabled, isTrue);
      expect(controls.buttonLabel, '停止');
      expect(controls.configEditable, isFalse);
    }
  });

  test('stopping disables connection actions', () {
    final controls = controlsFor(RuntimePhase.stopping);
    expect(controls.action, RuntimeConnectionAction.none);
    expect(controls.buttonEnabled, isFalse);
    expect(controls.buttonLabel, '停止');
    expect(controls.configEditable, isFalse);
  });

  test('failed enables retry and profile editing', () {
    final controls = controlsFor(RuntimePhase.failed);
    expect(controls.action, RuntimeConnectionAction.retry);
    expect(controls.buttonEnabled, isTrue);
    expect(controls.buttonLabel, '重试');
    expect(controls.configEditable, isTrue);
  });

  test('unknown exposes force stop', () {
    final controls = controlsFor(RuntimePhase.unknown);
    expect(controls.action, RuntimeConnectionAction.forceStop);
    expect(controls.buttonEnabled, isTrue);
    expect(controls.buttonLabel, '强制停止');
    expect(controls.configEditable, isFalse);
  });
}
