/// How the pipeline output is scaled into the view.
enum BevFit {
  /// Whole image, letterboxed or pillarboxed.
  contain,

  /// Fills the view, cropping the image.
  cover,

  /// Stretched to the view.
  fill,
}

/// The surround blend backends, as the CLI's `--blend`.
enum BevBlend { feather, pyramid, coverage }

/// What a BevView renders.
///
/// Each source encodes to the native creation params (ihs/bev_params.h), which
/// mirror the CLI's flags -- so a command line that works is a source that
/// works. Paths are read by the process running the shell; prefix one with
/// `asset:` to resolve it inside the app's flutter_assets.
sealed class BevSource {
  const BevSource();

  /// `key=value` pairs, in order. Keys may repeat.
  List<MapEntry<String, String>> toParams();
}

/// The live libcamera path: one camera, NV12, through the red-tint + parking
/// grid shader.
final class BevCameraSource extends BevSource {
  const BevCameraSource({
    this.index = 0,
    this.width,
    this.height,
    this.fps,
    this.tuningFile,
  });

  final int index;

  /// Requested stream size; libcamera may adjust it. Default 1640x1232.
  final int? width;
  final int? height;

  /// Pins the sensor's frame duration, capping the exposure auto-exposure may
  /// choose. Null leaves the duration to the IPA, which lengthens the frame to
  /// gather light — so the same scene streams slower in a dim room than a lit
  /// one, and two runs are no longer comparable. Set it when the rate matters
  /// more than the noise, as it does for a surround view.
  final double? fps;

  /// IPA tuning JSON, as `--tuning-file`. Process-wide: only the first camera
  /// view's value takes effect.
  final String? tuningFile;

  @override
  List<MapEntry<String, String>> toParams() => [
    const MapEntry('mode', 'camera'),
    MapEntry('camera_index', '$index'),
    if (width != null) MapEntry('width', '$width'),
    if (height != null) MapEntry('height', '$height'),
    if (fps != null) MapEntry('camera_fps', '$fps'),
    if (tuningFile != null) MapEntry('tuning_file', tuningFile!),
  ];
}

/// A camera pose on the ground plane, for single-file BEV (`--bev`).
final class BevGroundPose {
  const BevGroundPose({
    this.cameraHeight = 1.2,
    this.pitch = -30.0,
    this.yaw = 0.0,
    this.cameraY = 0.0,
    this.pxPerMeter = 100.0,
  });

  final double cameraHeight;
  final double pitch;
  final double yaw;
  final double cameraY;
  final double pxPerMeter;
}

/// One HEVC recording (`--file`), optionally warped to a forward BEV.
final class BevFileSource extends BevSource {
  const BevFileSource(this.path, {this.bev});

  final String path;

  /// When set, renders the forward bird's-eye view from this pose.
  final BevGroundPose? bev;

  @override
  List<MapEntry<String, String>> toParams() => [
    const MapEntry('mode', 'file'),
    MapEntry('file', path),
    if (bev case final pose?) ...[
      const MapEntry('bev', '1'),
      MapEntry('cam_h', '${pose.cameraHeight}'),
      MapEntry('pitch', '${pose.pitch}'),
      MapEntry('yaw', '${pose.yaw}'),
      MapEntry('cam_y', '${pose.cameraY}'),
      MapEntry('px_per_m', '${pose.pxPerMeter}'),
    ],
  ];
}

/// The car icon composited over the surround canvas's blind centre.
final class BevCarIcon {
  const BevCarIcon(this.path, {this.width, this.length, this.x, this.y});

  final String path;

  /// Meters; each falls back to the config file's value when null.
  final double? width;
  final double? length;
  final double? x;
  final double? y;
}

/// The N-camera surround view (`--src`, or the named `--forward-*` /
/// `--backward-*` pairs), with per-slot calibration from a bev_config.ini.
final class BevSurroundSource extends BevSource {
  /// Sources in config-slot order, as repeated `--src`.
  const BevSurroundSource(
    List<String> this.sources, {
    this.config = 'bev_config.ini',
    this.lensDir,
    this.blend = BevBlend.feather,
    this.pxPerMeter,
    this.carIcon,
  }) : forwardLeft = null,
       forwardRight = null,
       backwardLeft = null,
       backwardRight = null;

  /// The front/back pair rig: each recording pinned to its fixed config slot
  /// (0..3), so a backward-only run still reads slots 2 and 3.
  const BevSurroundSource.pairs({
    this.forwardLeft,
    this.forwardRight,
    this.backwardLeft,
    this.backwardRight,
    this.config = 'bev_config.ini',
    this.lensDir,
    this.blend = BevBlend.feather,
    this.pxPerMeter,
    this.carIcon,
  }) : sources = null;

  final List<String>? sources;
  final String? forwardLeft;
  final String? forwardRight;
  final String? backwardLeft;
  final String? backwardRight;

  final String config;

  /// Where `<lens_model>-lens.ini` files live; default the config's directory.
  final String? lensDir;
  final BevBlend blend;

  /// Overrides the config file's px_per_m.
  final double? pxPerMeter;
  final BevCarIcon? carIcon;

  @override
  List<MapEntry<String, String>> toParams() => [
    const MapEntry('mode', 'surround'),
    for (final src in sources ?? const <String>[]) MapEntry('src', src),
    if (forwardLeft != null) MapEntry('forward_left', forwardLeft!),
    if (forwardRight != null) MapEntry('forward_right', forwardRight!),
    if (backwardLeft != null) MapEntry('backward_left', backwardLeft!),
    if (backwardRight != null) MapEntry('backward_right', backwardRight!),
    MapEntry('config', config),
    if (lensDir != null) MapEntry('lens_dir', lensDir!),
    MapEntry('blend', blend.name),
    if (pxPerMeter != null) MapEntry('px_per_m', '$pxPerMeter'),
    if (carIcon case final icon?) ...[
      MapEntry('car_icon', icon.path),
      if (icon.width != null) MapEntry('car_width', '${icon.width}'),
      if (icon.length != null) MapEntry('car_length', '${icon.length}'),
      if (icon.x != null) MapEntry('car_x', '${icon.x}'),
      if (icon.y != null) MapEntry('car_y', '${icon.y}'),
    ],
  ];
}

/// Scrolling colour bars generated on the CPU -- for bringing a shell up with
/// no camera and no recording.
final class BevPatternSource extends BevSource {
  const BevPatternSource({
    this.width = 1280,
    this.height = 720,
    this.fps = 30,
  });

  final int width;
  final int height;

  /// Frames per second the pattern paces itself to. The other sources pace to
  /// their own input; this one has none. 0 runs it flat out, which is how you
  /// measure what the view can carry on a board.
  final double fps;

  @override
  List<MapEntry<String, String>> toParams() => [
    const MapEntry('mode', 'pattern'),
    MapEntry('width', '$width'),
    MapEntry('height', '$height'),
    MapEntry('pattern_fps', '$fps'),
  ];
}

/// The creation-params text for a view of [source].
String encodeBevParams(
  BevSource source, {
  BevFit fit = BevFit.contain,
  int ringSize = 4,
  bool scanout = false,
}) {
  final lines = <String>[
    for (final entry in source.toParams()) '${entry.key}=${entry.value}',
    'fit=${fit.name}',
    'ring_size=$ringSize',
    if (scanout) 'scanout=1',
  ];
  for (final line in lines) {
    // One line per pair is the whole format; a newline in a value would split it.
    if (line.contains('\n')) {
      throw ArgumentError.value(line, 'source', 'values cannot contain newlines');
    }
  }
  return '${lines.join('\n')}\n';
}
