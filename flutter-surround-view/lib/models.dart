// Domain types for the surround-view UI.
//
// Layout mock: nothing here talks to the backend yet. When the gRPC control
// channel lands, these mirror the enums in `surroundview.proto` and the screen
// state is replaced by whatever `WatchState` streams back.

enum ViewMode {
  grid,
  front,
  rear,
  left,
  right,
  bev;

  String get label => switch (this) {
        ViewMode.grid => 'Grid',
        ViewMode.front => 'Front',
        ViewMode.rear => 'Rear',
        ViewMode.left => 'Left',
        ViewMode.right => 'Right',
        ViewMode.bev => 'BEV',
      };

  /// The single camera this mode shows, or null for grid / BEV.
  CameraId? get asCamera => switch (this) {
        ViewMode.front => CameraId.front,
        ViewMode.rear => CameraId.rear,
        ViewMode.left => CameraId.left,
        ViewMode.right => CameraId.right,
        _ => null,
      };
}

enum CameraId {
  front,
  rear,
  left,
  right;

  String get label => switch (this) {
        CameraId.front => 'Front',
        CameraId.rear => 'Rear',
        CameraId.left => 'Left',
        CameraId.right => 'Right',
      };

  String get asset => 'assets/$name.png';
}

enum CameraOverlay { guidelines, distanceGrid }

enum CalAxis { x, y, yaw }

/// Per-camera calibration offset the left-rail nudge buttons accumulate.
class CalDelta {
  CalDelta({this.x = 0, this.y = 0, this.yaw = 0});

  double x;
  double y;
  double yaw;
}
