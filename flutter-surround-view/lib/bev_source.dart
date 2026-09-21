import 'dart:io';

import 'package:bev_view/bev_view.dart';

BevSource surroundSourceFromEnvironment() {
  final Map<String, String> env = Platform.environment;
  String? get(String key) {
    final String? value = env[key];
    return value == null || value.isEmpty ? null : value;
  }

  final int cameras = int.tryParse(get('BEV_CAMERAS') ?? '') ?? 2;
  return BevSurroundSource(
    <String>[for (int i = 0; i < cameras; i++) 'camera:$i'],
    config: get('BEV_CONFIG') ?? 'bev_config.ini',
    lensDir: get('BEV_LENS_DIR'),
    pxPerMeter: double.tryParse(get('BEV_PX_PER_M') ?? ''),
    fps: double.tryParse(get('BEV_CAM_FPS') ?? '') ?? 30,
    width: int.tryParse(get('BEV_WIDTH') ?? ''),
    height: int.tryParse(get('BEV_HEIGHT') ?? ''),
  );
}

BevSource cameraSourceFromEnvironment(int index) {
  final Map<String, String> env = Platform.environment;
  return BevCameraSource(
    index: index,
    fps: double.tryParse(env['BEV_CAM_FPS'] ?? '') ?? 30,
    width: int.tryParse(env['BEV_WIDTH'] ?? ''),
    height: int.tryParse(env['BEV_HEIGHT'] ?? ''),
  );
}

BevSource birdsEyeSourceFromEnvironment(int index) {
  final Map<String, String> env = Platform.environment;
  String? get(String key) {
    final String? value = env[key];
    return value == null || value.isEmpty ? null : value;
  }

  return BevSurroundSource(
    <String>['camera:$index'],
    config: get('BEV_CONFIG') ?? 'bev_config.ini',
    lensDir: get('BEV_LENS_DIR'),
    pxPerMeter: double.tryParse(get('BEV_PX_PER_M') ?? ''),
    fps: double.tryParse(get('BEV_CAM_FPS') ?? '') ?? 30,
    width: int.tryParse(get('BEV_WIDTH') ?? ''),
    height: int.tryParse(get('BEV_HEIGHT') ?? ''),
  );
}
