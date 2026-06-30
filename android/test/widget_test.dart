// This is a basic Flutter widget test.
//
// To perform an interaction with a widget in your test, use the WidgetTester
// utility in the flutter_test package. For example, you can send tap and scroll
// gestures. You can also use WidgetTester to find child widgets in the widget
// tree, read text, and verify that the values of widget properties are correct.

import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:openppp2_mobile/main.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  const vpnChannel = MethodChannel('supersocksr.ppp/vpn');
  const vpnEventsChannel = MethodChannel('supersocksr.ppp/vpn_events');

  setUp(() {
    SharedPreferences.setMockInitialValues({});
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(vpnChannel, (call) async {
      switch (call.method) {
        case 'getState':
          return 0;
        case 'getStatistics':
          return '{}';
        case 'getPathAwareness':
          return '{}';
        case 'getPathAwarenessAgeMs':
          return -1;
        case 'getLinkState':
          return 6;
        case 'readLog':
        case 'getLogPath':
          return '';
        case 'getVpnHeartbeatAgeMs':
          return -1;
        case 'clearLog':
        case 'disconnect':
          return true;
      }
      return null;
    });
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(vpnEventsChannel, (call) async => null);
  });

  tearDown(() {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(vpnChannel, null);
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(vpnEventsChannel, null);
  });

  testWidgets('App shell renders renamed mobile navigation',
      (WidgetTester tester) async {
    await tester.pumpWidget(const OpenPPP2App());
    await tester.pump();

    expect(find.text('未连接'), findsWidgets);
    expect(find.text('主页'), findsOneWidget);
    expect(find.text('服务器'), findsOneWidget);
    expect(find.text('参数'), findsOneWidget);
    expect(find.text('设置'), findsOneWidget);
    expect(find.text('启动参数'), findsNothing);
    expect(find.text('配置文件'), findsNothing);
  });

  testWidgets('Server and settings tabs expose Android server and OTEL UI',
      (WidgetTester tester) async {
    await tester.pumpWidget(const OpenPPP2App());
    await tester.pump();

    await tester.tap(find.text('服务器'));
    await tester.pump();

    expect(find.text('添加单个服务器'), findsWidgets);
    expect(find.text('导入远程订阅'), findsOneWidget);

    await tester.tap(find.text('设置'));
    await tester.pump();

    expect(find.text('OPENPPP2'), findsWidgets);
    expect(find.text('OpenTelemetry'), findsOneWidget);
    expect(find.text('路径感知'), findsOneWidget);
    expect(find.text('启用 OTEL'), findsOneWidget);
    expect(find.text('OTLP Endpoint'), findsOneWidget);
  });
}
