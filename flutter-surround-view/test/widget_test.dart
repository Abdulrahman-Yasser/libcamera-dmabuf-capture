import 'package:flutter_test/flutter_test.dart';

import 'package:surround_view/controls.dart';
import 'package:surround_view/main.dart';
import 'package:surround_view/viewport.dart';

void main() {
  testWidgets('360 degree is full screen with no BEV pane', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(const SurroundViewApp());

    expect(find.byType(TopToolbar), findsOneWidget);
    expect(find.byType(Surround360Placeholder), findsOneWidget);
    expect(find.byType(BevPlaceholder), findsNothing);
    expect(find.byType(CameraBox), findsNothing);
  });

  testWidgets('tapping a toolbar button switches to the 70/30 split', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(const SurroundViewApp());

    await tester.tap(find.text('Front camera'));
    await tester.pumpAndSettle();

    // 1 single feed in the controlled view + 4 BEV thumbnails.
    expect(find.byType(CameraBox), findsNWidgets(5));
    expect(find.byType(BevPlaceholder), findsOneWidget);
  });

  testWidgets(
      'tapping an already-selected camera button cycles its sub-view', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(const SurroundViewApp());

    await tester.tap(find.text('Front camera'));
    await tester.pumpAndSettle();
    expect(find.text('Front view'), findsOneWidget);
    expect(find.byType(CameraBox), findsNWidgets(5)); // + 4 BEV thumbnails

    await tester.tap(find.text('Front camera'));
    await tester.pumpAndSettle();
    expect(find.text('Front camera · 2D'), findsOneWidget);
    expect(find.text('Front - Calibrated 2D placeholder'), findsOneWidget);
    expect(find.byType(CameraBox), findsNWidgets(4)); // just the BEV thumbnails

    await tester.tap(find.text('Front camera · 2D'));
    await tester.pumpAndSettle();
    expect(find.text('Front camera · BEV'), findsOneWidget);
    expect(find.text('Front - BEV placeholder'), findsOneWidget);

    await tester.tap(find.text('Front camera · BEV'));
    await tester.pumpAndSettle();
    expect(find.text('Front view'), findsOneWidget);
    expect(find.byType(CameraBox), findsNWidgets(5));
  });

  testWidgets(
      'leaving a camera and coming back resets its sub-view to normal', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(const SurroundViewApp());

    // Front camera, cycled up to BEV.
    await tester.tap(find.text('Front camera'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Front camera'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Front camera · 2D'));
    await tester.pumpAndSettle();
    expect(find.text('Front camera · BEV'), findsOneWidget);

    // Switch away, then back to front, should be back to normal, not BEV.
    await tester.ensureVisible(find.text('Back camera'));
    await tester.tap(find.text('Back camera'));
    await tester.pumpAndSettle();
    await tester.ensureVisible(find.text('Front camera'));
    await tester.tap(find.text('Front camera'));
    await tester.pumpAndSettle();
    expect(find.text('Front camera'), findsOneWidget);
    expect(find.text('Front view'), findsOneWidget);
  });

  testWidgets(
      'side views cycles left and right together onto the same sub-view', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(const SurroundViewApp());

    await tester.ensureVisible(find.text('Side views'));
    await tester.tap(find.text('Side views'));
    await tester.pumpAndSettle();
    expect(find.byType(CameraBox), findsNWidgets(6)); // left+right + 4 BEV

    await tester.ensureVisible(find.text('Side views'));
    await tester.tap(find.text('Side views'));
    await tester.pumpAndSettle();
    expect(find.text('Left - Calibrated 2D placeholder'), findsOneWidget);
    expect(find.text('Right - Calibrated 2D placeholder'), findsOneWidget);
    expect(find.byType(CameraBox), findsNWidgets(4)); // just the BEV thumbnails
  });

  testWidgets('rear/front toggle is full screen and flips on each tap', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(const SurroundViewApp());

    await tester.ensureVisible(find.text('Rear/Front cameras'));
    await tester.tap(find.text('Rear/Front cameras'));
    await tester.pumpAndSettle();
    expect(find.text('Front view'), findsOneWidget);
    expect(find.byType(BevPlaceholder), findsNothing);
    expect(find.byType(CameraBox), findsOneWidget);

    await tester.tap(find.text('Rear/Front cameras'));
    await tester.pumpAndSettle();
    expect(find.text('Rear view'), findsOneWidget);

    await tester.tap(find.text('Rear/Front cameras'));
    await tester.pumpAndSettle();
    expect(find.text('Front view'), findsOneWidget);
  });

  testWidgets('overflow Calibrate reveals and hides the calibration panel', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(const SurroundViewApp());
    expect(find.byType(CalibrationPanel), findsNothing);

    await tester.tap(find.byTooltip('More'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Calibrate'), warnIfMissed: false);
    await tester.pumpAndSettle();
    expect(find.byType(CalibrationPanel), findsOneWidget);

    await tester.tap(find.byTooltip('More'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Calibrate'), warnIfMissed: false);
    await tester.pumpAndSettle();
    expect(find.byType(CalibrationPanel), findsNothing);
  });
}
