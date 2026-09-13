// Smoke tests for the layout mock: the L-shaped chrome renders, the view-mode
// buttons switch the viewport, and Calibrate reveals its panel.

import 'package:flutter_test/flutter_test.dart';

import 'package:surround_view/controls.dart';
import 'package:surround_view/main.dart';
import 'package:surround_view/viewport.dart';

void main() {
  testWidgets('renders the rail, the bottom bar and the 2x2 grid', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(const SurroundViewApp());

    expect(find.byType(LeftRail), findsOneWidget);
    expect(find.byType(BottomBar), findsOneWidget);
    expect(find.byType(CameraBox), findsNWidgets(4));
  });

  testWidgets('tapping BEV switches the viewport', (WidgetTester tester) async {
    await tester.pumpWidget(const SurroundViewApp());

    await tester.ensureVisible(find.text('BEV'));
    await tester.tap(find.text('BEV'));
    await tester.pumpAndSettle();

    expect(find.byType(BevPlaceholder), findsOneWidget);
    expect(find.byType(CameraBox), findsNWidgets(4)); // four BEV thumbnails
  });

  testWidgets('Calibrate reveals and hides the calibration panel', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(const SurroundViewApp());
    expect(find.byType(CalibrationPanel), findsNothing);

    await tester.tap(find.text('Calibrate'));
    await tester.pumpAndSettle();
    expect(find.byType(CalibrationPanel), findsOneWidget);

    await tester.tap(find.text('Calibrate'));
    await tester.pumpAndSettle();
    expect(find.byType(CalibrationPanel), findsNothing);
  });
}
