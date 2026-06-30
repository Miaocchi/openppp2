import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:openppp2_mobile/services/profile_store.dart';

void main() {
  group('ProfileStore path awareness defaults', () {
    test('observes paths by default without enabling multi-network routing', () {
      expect(ProfileStore.defaultOptions['pathAwarenessEnabled'], isTrue);
      expect(ProfileStore.defaultOptions['multiNetworkEnabled'], isFalse);
      expect(ProfileStore.defaultOptions['multiNetworkMode'], 'handover');
      expect(ProfileStore.defaultOptions['multiNetworkCellularPolicy'], 'system');
      expect(ProfileStore.defaultOptions['multiNetworkPrimary'], 'auto');
    });
  });

  group('ProfileStore telemetry merge', () {
    test('injects Android OTEL settings into effective AppConfiguration JSON',
        () {
      final merged = ProfileStore.effectiveJson(
        ProfileStore.defaultJson,
        ProfileStore.defaultOptions,
        telemetry: {
          'enabled': true,
          'endpoint': 'https://otel.example.com',
          'level': 3,
          'metricsEnabled': true,
          'spansEnabled': true,
          'logFile': '/sdcard/Download/openppp2-otel.log',
          'consoleLog': true,
          'consoleMetric': true,
          'consoleSpan': false,
        },
      );

      final root = jsonDecode(merged) as Map<String, dynamic>;
      final telemetry = root['telemetry'] as Map<String, dynamic>;
      expect(telemetry['enabled'], isTrue);
      expect(telemetry['endpoint'], 'https://otel.example.com');
      expect(telemetry['level'], 3);
      expect(telemetry['count'], isTrue);
      expect(telemetry['span'], isTrue);
      expect(telemetry['log-file'], '/sdcard/Download/openppp2-otel.log');
      expect(telemetry['console-log'], isTrue);
      expect(telemetry['console-metric'], isTrue);
      expect(telemetry['console-span'], isFalse);
    });

    test('keeps native telemetry disabled by default', () {
      final merged = ProfileStore.effectiveJson(
        ProfileStore.defaultJson,
        ProfileStore.defaultOptions,
        telemetry: ProfileStore.defaultTelemetrySettings,
      );

      final root = jsonDecode(merged) as Map<String, dynamic>;
      final telemetry = root['telemetry'] as Map<String, dynamic>;
      expect(telemetry['enabled'], isFalse);
      expect(telemetry['endpoint'], '');
      expect(telemetry['level'], 1);
      expect(telemetry['count'], isFalse);
      expect(telemetry['span'], isFalse);
    });
  });
}
