// Native-assets build hook. Builds libbev_view.so -- this repository's pipeline
// plus its ivi-homescreen platform-view seam (ihs/) -- from the top-level
// CMakeLists.txt, and publishes it under the id lib/src/ffi/bev_ffi.dart names
// in its @DefaultAsset, so the @Native externals resolve against it.
//
// The hook runner strips the environment, so configuration comes from
// user-defines in the consuming app's pubspec.yaml:
//
//   hooks:
//     user_defines:
//       bev_view:
//         ihs_dir: ../../../ivi-homescreen  # checkout; default: probed beside the repo
//         ihs_build_dir: cmake-build-debug  # its build dir; default: first with shared/ built
//         enable_vulkan: true               # allocate on the shell's Vulkan device (default true)
//         build_type: Release               # CMAKE_BUILD_TYPE (default Release)
//         cmake_toolchain_file: <path>      # cross builds
//         clear_ambient_flags: true         # drop CFLAGS/CXXFLAGS/LDFLAGS
//
// Relative directories resolve against this package. With no ihs_dir and no
// sibling checkout, CMake falls back to an installed ivi-homescreen-shared.

import 'dart:io';

import 'package:code_assets/code_assets.dart';
import 'package:hooks/hooks.dart';

const _clearedFlagVars = {'CFLAGS': '', 'CXXFLAGS': '', 'LDFLAGS': ''};

void main(List<String> args) async {
  await build(args, (input, output) async {
    if (!input.config.buildCodeAssets) return;

    // flutter/bev_view -> the repository root, which holds the CMake project.
    final repoRoot = input.packageRoot.resolve('../../').toFilePath();
    final buildDir = input.outputDirectory.resolve('cmake/').toFilePath();
    await Directory(buildDir).create(recursive: true);

    final ihsDir = _dir(input, 'ihs_dir');
    final ihsBuildDir = _string(input, 'ihs_build_dir');
    final toolchain = _dir(input, 'cmake_toolchain_file');
    final buildType = _string(input, 'build_type') ?? 'Release';
    final enableVulkan = _flag(input, 'enable_vulkan', defaultValue: true);
    final clearFlags = _flag(input, 'clear_ambient_flags');
    final env = clearFlags ? _clearedFlagVars : null;

    // Always (re)configure: it is cheap next to the build, and a cache left by
    // an earlier configuration would silently ignore a changed user-define.
    await _run('cmake', [
      '-S', repoRoot,
      '-B', buildDir,
      '-DCMAKE_BUILD_TYPE=$buildType',
      '-DENABLE_IHS_PV=ON',
      // The --preview window is the CLI's; the view never needs a compositor
      // connection of its own.
      '-DENABLE_WAYLAND_PREVIEW=OFF',
      '-DENABLE_IHS_PV_VULKAN=${enableVulkan ? 'ON' : 'OFF'}',
      if (ihsDir != null) '-DIHS_DIR=$ihsDir',
      if (ihsBuildDir != null)
        '-DIHS_BUILD_DIR=${ihsBuildDir.startsWith('/') || ihsDir == null ? ihsBuildDir : '$ihsDir/$ihsBuildDir'}',
      if (toolchain != null) '-DCMAKE_TOOLCHAIN_FILE=$toolchain',
      if (!File('${buildDir}CMakeCache.txt').existsSync() && await _which('ninja')) ...[
        '-G',
        'Ninja',
      ],
    ], environment: env);

    await _run(
      'cmake',
      ['--build', buildDir, '--target', 'bev_view', '--parallel'],
      environment: env,
    );

    final lib = File('${buildDir}libbev_view.so');
    if (!lib.existsSync()) {
      throw StateError('libbev_view.so not found at ${lib.path}');
    }
    output.assets.code.add(
      CodeAsset(
        package: input.packageName,
        name: 'src/ffi/bev_ffi.dart',
        linkMode: DynamicLoadingBundled(),
        file: lib.uri,
      ),
    );

    // Re-run when any native source the library is built from changes.
    for (final dir in [repoRoot, '${repoRoot}ihs']) {
      for (final entity in Directory(dir).listSync()) {
        if (entity is File &&
            (entity.path.endsWith('.cpp') || entity.path.endsWith('.h'))) {
          output.dependencies.add(entity.uri);
        }
      }
    }
    output.dependencies.add(Uri.file('${repoRoot}CMakeLists.txt'));
  });
}

String? _string(BuildInput input, String define) {
  final value = input.userDefines[define];
  return value == null || '$value'.isEmpty ? null : '$value';
}

/// [_string] resolved against this package: the hook protocol does not fix a
/// working directory, so a relative path would otherwise mean different things
/// under `dart` and `flutter`.
String? _dir(BuildInput input, String define) {
  final value = _string(input, define);
  if (value == null) return null;
  return value.startsWith('/')
      ? value
      : input.packageRoot.resolve(value).toFilePath();
}

/// YAML gives a bool, `--define` a string; '', '0' and 'false' are off.
bool _flag(BuildInput input, String define, {bool defaultValue = false}) {
  final value = input.userDefines[define];
  if (value == null) return defaultValue;
  return value == true || (value is String && value != '' && value != '0' && value != 'false');
}

Future<bool> _which(String exe) async {
  try {
    final r = await Process.run('sh', ['-c', 'command -v $exe']);
    return r.exitCode == 0;
  } on ProcessException {
    return false;
  }
}

Future<void> _run(
  String exe,
  List<String> args, {
  Map<String, String>? environment,
}) async {
  final proc = await Process.start(
    exe,
    args,
    environment: environment,
    mode: ProcessStartMode.inheritStdio,
  );
  final code = await proc.exitCode;
  if (code != 0) {
    throw ProcessException(exe, args, 'exit code $code', code);
  }
}
