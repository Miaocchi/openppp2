// This is a basic Flutter widget test.
//
// To perform an interaction with a widget in your test, use the WidgetTester
// utility in the flutter_test package. For example, you can send tap and scroll
// gestures. You can also use WidgetTester to find child widgets in the widget
// tree, read text, and verify that the values of widget properties are correct.

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:openppp2_mobile/main.dart';

void main() {
  testWidgets('App shell renders home and settings tabs', (WidgetTester tester) async {
    // Build our app and trigger a frame.
    await tester.pumpWidget(const OpenPPP2App());

    // Verify that the home tab renders the current connection state.
    expect(find.text('Not Connected'), findsOneWidget);
    expect(find.text('主页'), findsOneWidget);

    // Switch to the settings tab from the bottom navigation.
    await tester.tap(find.text('设置'));
    await tester.pumpAndSettle();

    // Verify that the settings page is displayed.
    expect(find.text('OPENPPP2'), findsOneWidget);
    expect(find.text('显示调试面板'), findsOneWidget);
  });
}
