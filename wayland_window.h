#pragma once

#include "egl_context.h"

#include <memory>

// Live preview window: an AGL-shell background surface that shows whatever
// GpuRenderer last rendered into its FBO texture. Opt-in (--preview) — the
// default headless GBM/surfaceless pipeline never depends on this.
//
// Usage (see main.cpp):
//   WaylandPreviewWindow preview;
//   if (!preview.init(1280, 720)) { ... }
//   const EGLState &egl = preview.egl_state();     // hand to GpuRenderer::init()
//   ...
//   renderer.render_frame(frame);                  // pass 1, GpuRenderer's own FBO
//   preview.pump_events();
//   preview.present(renderer.fbo_texture(),          // pass 2: composite + swap
//                   renderer.width(), renderer.height());
class WaylandPreviewWindow {
public:
    WaylandPreviewWindow();
    ~WaylandPreviewWindow();

    WaylandPreviewWindow(const WaylandPreviewWindow &) = delete;
    WaylandPreviewWindow &operator=(const WaylandPreviewWindow &) = delete;

    // Connects to the compositor, performs the xdg_wm_base/agl_shell
    // background-surface handshake, and creates a windowed EGL context sized
    // width x height. Returns false on any failure (no compositor running,
    // not an AGL-shell compositor, EGL setup failure, ...).
    bool init(int width, int height);

    // Non-blocking: flushes queued requests and dispatches whatever events
    // are already buffered client-side (resize, close, presentation
    // feedback). Never blocks on the socket — call once per render-loop
    // iteration so it never stalls frame delivery.
    void pump_events();

    // Pass 2 of the two-pass render: composites `scene_tex` (GpuRenderer's
    // FBO color texture, `scene_w` x `scene_h` pixels) onto the window,
    // letterboxed/pillarboxed to preserve its aspect ratio when it doesn't
    // match the window's, arms wp_presentation feedback, and swaps buffers.
    void present(unsigned int scene_tex, int scene_w, int scene_h);

    // False once the compositor has asked the toplevel to close.
    bool running() const;

    // EGLState produced by this window's setup_egl_wayland() call — hand
    // this to GpuRenderer::init() so rendering happens on the same context
    // the window presents from.
    const EGLState &egl_state() const;

    // Tears down EGL, the Wayland objects, and disconnects — in dependency
    // order. Idempotent. Called by the destructor if not called explicitly.
    void cleanup();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
