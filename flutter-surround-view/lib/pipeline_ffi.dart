import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';

import 'package:ffi/ffi.dart';

typedef _CreateC = Pointer<Void> Function(Pointer<Utf8>, Pointer<Utf8>);
typedef _CreateD = Pointer<Void> Function(Pointer<Utf8>, Pointer<Utf8>);
typedef _StartC = Int32 Function(Pointer<Void>);
typedef _StartD = int Function(Pointer<Void>);
typedef _CalibC = Int32 Function(Pointer<Void>, Int32, Double, Double, Double);
typedef _CalibD = int Function(Pointer<Void>, int, double, double, double);
typedef _SnapC = Int32 Function(Pointer<Void>, Pointer<Utf8>);
typedef _SnapD = int Function(Pointer<Void>, Pointer<Utf8>);
typedef _RegC = Int32 Function(Pointer<Void>, Pointer<Utf8>);
typedef _RegD = int Function(Pointer<Void>, Pointer<Utf8>);
typedef _VoidC = Void Function(Pointer<Void>);
typedef _VoidD = void Function(Pointer<Void>);

DynamicLibrary _open() {
  final String path =
      Platform.environment['PIPELINE_LIB'] ?? 'libpipeline_core.so';
  return DynamicLibrary.open(path);
}

int _startInIsolate(int handle) {
  final _StartD start =
      _open().lookupFunction<_StartC, _StartD>('pipeline_start');
  return start(Pointer<Void>.fromAddress(handle));
}

class Pipeline {
  Pipeline._(this._lib, this._handle);

  final DynamicLibrary _lib;
  final Pointer<Void> _handle;
  bool _running = false;

  static Pipeline? open({String? configPath, String? tuningFile}) {
    try {
      final DynamicLibrary lib = _open();
      final _CreateD create =
          lib.lookupFunction<_CreateC, _CreateD>('pipeline_create');
      final Pointer<Utf8> cfg =
          configPath == null ? nullptr : configPath.toNativeUtf8();
      final Pointer<Utf8> tun =
          tuningFile == null ? nullptr : tuningFile.toNativeUtf8();
      final Pointer<Void> handle = create(cfg, tun);
      if (cfg != nullptr) malloc.free(cfg);
      if (tun != nullptr) malloc.free(tun);
      return handle == nullptr ? null : Pipeline._(lib, handle);
    } on ArgumentError {
      return null;
    }
  }

  bool get running => _running;

  Future<bool> start() async {
    final int address = _handle.address;
    final int rc = await Isolate.run(() => _startInIsolate(address));
    _running = rc == 0;
    return _running;
  }

  bool setCalibration(int slot, double dxMeters, double dyMeters, double yawDeg) {
    if (!_running) return false;
    final _CalibD fn =
        _lib.lookupFunction<_CalibC, _CalibD>('pipeline_set_calibration');
    return fn(_handle, slot, dxMeters, dyMeters, yawDeg) == 0;
  }

  bool saveSnapshot(String path) {
    if (!_running) return false;
    final _SnapD fn =
        _lib.lookupFunction<_SnapC, _SnapD>('pipeline_save_snapshot');
    final Pointer<Utf8> p = path.toNativeUtf8();
    final int rc = fn(_handle, p);
    malloc.free(p);
    return rc == 0;
  }

  bool registerPlatformView(String viewType) {
    final _RegD fn = _lib
        .lookupFunction<_RegC, _RegD>('pipeline_register_platform_view');
    final Pointer<Utf8> p = viewType.toNativeUtf8();
    final int rc = fn(_handle, p);
    malloc.free(p);
    return rc == 0;
  }

  void dispose() {
    final _VoidD stop = _lib.lookupFunction<_VoidC, _VoidD>('pipeline_stop');
    final _VoidD destroy =
        _lib.lookupFunction<_VoidC, _VoidD>('pipeline_destroy');
    stop(_handle);
    destroy(_handle);
    _running = false;
  }
}
