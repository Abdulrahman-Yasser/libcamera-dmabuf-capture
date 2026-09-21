enum ViewMode {
  surround360,
  front,
  rear,
  left,
  right,
  sideViews,
  frontRear;

  String get label => switch (this) {
        ViewMode.surround360 => '360°',
        ViewMode.front => 'Front',
        ViewMode.rear => 'Rear',
        ViewMode.left => 'Left',
        ViewMode.right => 'Right',
        ViewMode.sideViews => 'Side views',
        ViewMode.frontRear => 'Front/Rear',
      };

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

enum CameraSubView {
  normal,
  calibrated2d,
  bev;

  String get label => switch (this) {
        CameraSubView.normal => 'Normal (360°)',
        CameraSubView.calibrated2d => 'Calibrated 2D',
        CameraSubView.bev => 'BEV',
      };

  String get shortLabel => switch (this) {
        CameraSubView.normal => '360°',
        CameraSubView.calibrated2d => '2D',
        CameraSubView.bev => 'BEV',
      };

  CameraSubView get next => switch (this) {
        CameraSubView.normal => CameraSubView.calibrated2d,
        CameraSubView.calibrated2d => CameraSubView.bev,
        CameraSubView.bev => CameraSubView.normal,
      };
}

enum CameraOverlay { guidelines, distanceGrid }

enum CalAxis { x, y, yaw }

class CalDelta {
  CalDelta({this.x = 0, this.y = 0, this.yaw = 0});

  double x;
  double y;
  double yaw;
}
