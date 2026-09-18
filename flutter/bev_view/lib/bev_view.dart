/// The libcamera-dmabuf-capture surround-view pipeline as an ivi-homescreen
/// platform view.
///
/// [BevView] creates a "views/bev-view" platform view whose native producer
/// (libbev_view.so, built by this package's hook) renders a [BevSource] and
/// hands the shell a dma-buf per frame. [BevViewController] reads its counters
/// and drives the runtime controls the CLI binds to keys.
library;

export 'src/bev_view_widget.dart' show BevView, BevViewCreatedCallback;
export 'src/controller.dart';
export 'src/source.dart';
