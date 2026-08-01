// WaylandPreviewWindow — an AGL-shell background client that shows whatever
// GpuRenderer last rendered into its FBO texture.
//
// Adapted from wayland-cxx-scanner's own examples/agl-presentation-egl
// (bundled in extern/wayland-cxx-scanner): same xdg_wm_base/agl_shell
// background-surface handshake, same "render-to-texture then composite via a
// full-screen pass" idea (GLES2/3 has no glBlitFramebuffer). Two differences
// from that example:
//
//   1. Pass 1 (the actual scene) is GpuRenderer's job, not ours — we only do
//      Pass 2 (composite GpuRenderer's FBO texture onto the window).
//   2. Frame pacing: the example uses a wl_surface.frame callback chain
//      (push model, paced by the compositor). This project's render loops
//      are pull-model, driven by camera/file frame availability, so
//      pump_events() is a non-blocking dispatch instead — pacing comes from
//      eglSwapInterval(dpy, 1)-blocking eglSwapBuffers().
//
// IMPORTANT: set_background must come AFTER the first wl_surface.commit() —
// the compositor inspects the committed surface state (role) when
// processing set_background, and an uncommitted surface has no role yet.

// ── EGL/GLES headers (must precede any Wayland headers to avoid wl_display
//    redefinition issues on some EGL implementations) ───────────────────────
extern "C" {
#include <EGL/egl.h>
#include <GLES3/gl3.h>
}

#include "wayland_window.h"

// ── Generated C++ protocol headers ───────────────────────────────────────────
#include "agl_shell_client.hpp"          // namespace agl_shell::client
#include "presentation_time_client.hpp"  // namespace presentation_time::client
#include "wayland_client.hpp"            // namespace wayland::client
#include "xdg_shell_client.hpp"          // namespace xdg_shell::client

// ── System Wayland headers ───────────────────────────────────────────────────
extern "C" {
#include <wayland-client-protocol.h>  // wl_*_interface symbols
#include <wayland-egl.h>              // wl_egl_window_{create,destroy,resize}
}

// ── Framework headers ─────────────────────────────────────────────────────────
#include <wl/agl_shell.hpp>
#include <wl/client_helpers.hpp>
#include <wl/display.hpp>
#include <wl/present_feedback.hpp>
#include <wl/presentation.hpp>
#include <wl/raii.hpp>
#include <wl/registry.hpp>
#include <wl/wl_ptr.hpp>
#include <wl/xdg_shell.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#include <poll.h>

// ══════════════════════════════════════════════════════════════════════════
// wl_iface() definitions — core Wayland interfaces (no pre-built symbol is
// otherwise linked in for the traits the generated header declares).
// ══════════════════════════════════════════════════════════════════════════

namespace wayland::client {

const wl_interface &wl_callback_traits::wl_iface() noexcept {
    return wl_callback_interface;
}
const wl_interface &wl_compositor_traits::wl_iface() noexcept {
    return wl_compositor_interface;
}
const wl_interface &wl_surface_traits::wl_iface() noexcept {
    return wl_surface_interface;
}
const wl_interface &wl_output_traits::wl_iface() noexcept {
    return wl_output_interface;
}

}  // namespace wayland::client

namespace {

// ── Composite-pass shader ────────────────────────────────────────────────────
// A gl_VertexID-driven quad (no VBO needed, same array-of-constants style
// gpu_renderer.cpp's own kVS uses for its full-screen triangle) instead of a
// full-screen triangle, because the quad's corners need to move independently
// on each axis for letterbox/pillarbox (uScale). Samples GpuRenderer's FBO
// texture directly; no Y-flip needed here (GpuRenderer's fragment shader
// already corrects the DMA-BUF top-left source orientation into standard GL
// bottom-left texture orientation when it writes the FBO).
const char kQuadVS[] = R"glsl(
#version 300 es
uniform vec2 uScale;
out vec2 vUV;
void main() {
    const vec2 pos[4] = vec2[4](
        vec2(-1.0, -1.0),
        vec2( 1.0, -1.0),
        vec2(-1.0,  1.0),
        vec2( 1.0,  1.0)
    );
    vec2 p = pos[gl_VertexID];
    vUV = p * 0.5 + 0.5;
    gl_Position = vec4(p * uScale, 0.0, 1.0);
}
)glsl";

const char kQuadFS[] = R"glsl(
#version 300 es
precision mediump float;
uniform sampler2D uTex;
in  vec2 vUV;
out vec4 fragColor;
void main() {
    fragColor = texture(uTex, vUV);
}
)glsl";

GLuint compile_shader(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[preview] shader compile failed:\n%s\n", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLuint build_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[preview] program link failed:\n%s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

}  // namespace

// wl_compositor / wl_surface / wl_output have no events we need — concrete,
// listener-less handlers.
class WlCompositorHandler : public wayland::client::CWlCompositor<WlCompositorHandler> {};
class WlSurfaceHandler    : public wayland::client::CWlSurface<WlSurfaceHandler> {};
class WlOutputHandler     : public wayland::client::CWlOutput<WlOutputHandler> {};

// ══════════════════════════════════════════════════════════════════════════
// WaylandPreviewWindow::Impl
// ══════════════════════════════════════════════════════════════════════════

struct WaylandPreviewWindow::Impl {
    // ── Callbacks used by the CRTP handler templates ─────────────────────────
    // wl::XdgSurfaceHandler<App>::OnConfigure (xdg_shell.hpp) already calls
    // AckConfigure(serial) before invoking this -- do NOT ack again here.
    // Doing so double-acks every configure event (harmless-looking the first
    // time, since re-acking an already-acked serial 1 was apparently
    // tolerated, but a second configure event -- serial 2 -- getting
    // double-acked is what actually triggered
    // "xdg_wm_base error 4: Wrong configure serial: 2" on real hardware,
    // which kills the wl_display connection and cascades into
    // "eglInitialize failed: 0x3001" downstream.
    void OnXdgSurfaceConfigure(uint32_t /*serial*/) noexcept
    {
        configured_ = true;
    }

    void OnToplevelConfigure(int32_t w, int32_t h) noexcept
    {
        static constexpr int32_t kMaxDim = 16384;
        if (w > 0 && h > 0) {
            width_  = std::min(w, kMaxDim);
            height_ = std::min(h, kMaxDim);
            if (egl_window_)
                wl_egl_window_resize(egl_window_, width_, height_, 0, 0);
            std::printf("[preview] toplevel configure %dx%d\n", width_, height_);
        }
    }

    void OnToplevelClose() noexcept { running_ = false; }

    void OnAglBoundOk() noexcept
    {
        bound_state_ = BoundState::Ok;
        std::printf("[preview] agl_shell bound_ok\n");
    }
    void OnAglBoundFail() noexcept
    {
        bound_state_ = BoundState::Fail;
        std::fprintf(stderr, "[preview] agl_shell bound_fail — another shell "
                             "client is already active\n");
    }
    static void OnAglAppState(const char * /*app_id*/, uint32_t /*state*/) {}

    void OnPresented(const wl::PresentFeedback &fb) noexcept
    {
        ++presented_count_;
        latency_sum_ms_ += fb.latency_ms;
        if (fb.refresh_ns != 0u) last_refresh_ns_ = fb.refresh_ns;

        if (presented_count_ % kReportEveryFrames != 0u) return;

        double mean_ms   = latency_sum_ms_ / (double)kReportEveryFrames;
        double refresh_ms = (double)last_refresh_ns_ / 1.0e6;
        double hz = last_refresh_ns_ != 0u ? 1.0e9 / (double)last_refresh_ns_ : 0.0;
        std::printf("[preview] %u presented, mean commit->light %.2f ms, "
                    "refresh %.2f ms (%.1f Hz)\n",
                    presented_count_, mean_ms, refresh_ms, hz);
        latency_sum_ms_ = 0.0;
    }
    void OnDiscarded(uint32_t /*frame*/) noexcept {}

    // ── Setup sequence ────────────────────────────────────────────────────────
    bool connect_display()
    {
        if (!display_.Connect()) {
            std::fprintf(stderr, "[preview] wl_display_connect: %s\n", std::strerror(errno));
            return false;
        }
        return true;
    }

    bool scan_globals()
    {
        if (!registry_.Create(display_.Get())) {
            std::fprintf(stderr, "[preview] wl_display_get_registry failed\n");
            return false;
        }

        registry_.OnGlobal([this](wl::CRegistry & /*reg*/, uint32_t name,
                                  std::string_view iface, uint32_t ver) {
            using namespace wayland::client;
            using namespace xdg_shell::client;
            using namespace agl_shell::client;
            using namespace presentation_time::client;

            if (iface == wl_compositor_traits::interface_name) {
                compositor_name_ = name; compositor_ver_ = ver;
            } else if (iface == wl_output_traits::interface_name && !output_name_) {
                output_name_ = name; output_ver_ = ver;
            } else if (iface == xdg_wm_base_traits::interface_name) {
                xdg_wm_base_name_ = name; xdg_wm_base_ver_ = ver;
            } else if (iface == agl_shell_traits::interface_name) {
                agl_shell_name_ = name; agl_shell_ver_ = ver;
            } else if (iface == wp_presentation_traits::interface_name) {
                presentation_.Record(name, ver);
            }
        });

        if (!wl::RoundtripWithTimeout(display_.Get())) {
            std::fprintf(stderr, "[preview] timed out waiting for globals\n");
            return false;
        }
        if (!compositor_name_) {
            std::fprintf(stderr, "[preview] wl_compositor not advertised\n");
            return false;
        }
        if (!xdg_wm_base_name_) {
            std::fprintf(stderr, "[preview] xdg_wm_base not advertised "
                                 "(compositor missing xdg-shell support?)\n");
            return false;
        }
        if (!agl_shell_name_) {
            std::printf("[preview] agl_shell not advertised (not an AGL "
                        "compositor) — showing a regular top-level window "
                        "instead of an agl_shell background surface\n");
        }
        if (!output_name_) {
            std::fprintf(stderr, "[preview] no wl_output advertised\n");
            return false;
        }
        return true;
    }

    bool bind_globals()
    {
        using namespace wayland::client;
        using namespace xdg_shell::client;
        using namespace agl_shell::client;

        if (wl_proxy *raw = registry_.Bind<wl_compositor_traits>(
                compositor_name_, std::min(compositor_ver_, wl_compositor_traits::version))) {
            compositor_.Attach(raw);
        } else {
            std::fprintf(stderr, "[preview] wl_compositor bind failed\n");
            return false;
        }

        if (wl_proxy *raw = registry_.Bind<wl_output_traits>(
                output_name_, std::min(output_ver_, wl_output_traits::version))) {
            output_.Attach(raw);
        } else {
            std::fprintf(stderr, "[preview] wl_output bind failed\n");
            return false;
        }

        if (!wl::BindHandler<xdg_wm_base_traits>(registry_, xdg_wm_base_,
                                                 xdg_wm_base_name_, xdg_wm_base_ver_)) {
            std::fprintf(stderr, "[preview] xdg_wm_base bind failed\n");
            return false;
        }

        if (agl_shell_name_) {
            if (!wl::BindHandler<agl_shell_traits>(registry_, agl_shell_,
                                                   agl_shell_name_, agl_shell_ver_)) {
                std::fprintf(stderr, "[preview] agl_shell bind failed\n");
                return false;
            }
            agl_shell_.Get()->app_ = this;
        }

        if (!presentation_.Bind(registry_, this)) {
            std::fprintf(stderr, "[preview] wp_presentation bind failed\n");
            return false;
        }

        if (!wl::RoundtripWithTimeout(display_.Get())) {
            std::fprintf(stderr, "[preview] timed out waiting for bind events\n");
            return false;
        }

        if (!agl_shell_name_) return true; // no agl_shell to wait on

        if (bound_state_ == BoundState::Fail) return false;
        if (bound_state_ == BoundState::Waiting) {
            uint32_t bound_ver = std::min(agl_shell_ver_, agl_shell_traits::version);
            if (bound_ver >= 2u) {
                std::fprintf(stderr, "[preview] no bound event received from v%u "
                                     "compositor (compositor bug?)\n", bound_ver);
                return false;
            }
            std::printf("[preview] v1 compositor — proceeding without bound confirmation\n");
        }
        return true;
    }

    // Canonical xdg + agl_shell background-surface sequence. Critical
    // ordering: create surface -> empty commit -> set_background -> wait for
    // xdg_surface::configure -> agl_shell.ready().
    bool setup_shell()
    {
        using namespace wayland::client;
        using namespace xdg_shell::client;

        if (wl_proxy *raw = wl::construct<wl_surface_traits, wl_compositor_traits::Op::CreateSurface>(
                *compositor_.Get())) {
            surface_.Get()->_SetProxy(raw);
        } else {
            std::fprintf(stderr, "[preview] wl_compositor.create_surface failed\n");
            return false;
        }

        if (!wl::SetupHandler(xdg_surface_,
                              wl::construct<xdg_surface_traits, xdg_wm_base_traits::Op::GetXdgSurface>(
                                  *xdg_wm_base_.Get(), surface_.Get()->GetProxy()))) {
            std::fprintf(stderr, "[preview] xdg_wm_base.get_xdg_surface failed\n");
            return false;
        }
        xdg_surface_.Get()->app_ = this;

        if (!wl::SetupHandler(xdg_toplevel_,
                              wl::construct<xdg_toplevel_traits, xdg_surface_traits::Op::GetToplevel>(
                                  *xdg_surface_.Get()))) {
            std::fprintf(stderr, "[preview] xdg_surface.get_toplevel failed\n");
            return false;
        }
        xdg_toplevel_.Get()->app_ = this;
        xdg_toplevel_.Get()->SetTitle("libcamera-dmabuf-capture preview");
        xdg_toplevel_.Get()->SetAppId("org.agl.libcamera-dmabuf-capture-preview");

        // Only for the plain-toplevel fallback (no agl_shell) — an agl_shell
        // background surface already covers the whole output by definition,
        // so requesting fullscreen on top of that role is redundant (and
        // untested against agl-compositor); left as before on that path.
        if (!agl_shell_name_)
            xdg_toplevel_.Get()->SetFullscreen(output_.Get()->GetProxy());

        // Empty commit — establishes the xdg role in the committed state.
        // MUST precede set_background.
        surface_.Get()->Commit();

        if (agl_shell_name_) {
            agl_shell_.Get()->SetBackground(surface_.Get()->GetProxy(), output_.Get()->GetProxy());
            std::printf("[preview] background surface registered with agl_shell\n");
        } else {
            std::printf("[preview] no agl_shell — showing as a regular top-level window\n");
        }

        while (!configured_) {
            if (!wl::RoundtripWithTimeout(display_.Get())) {
                std::fprintf(stderr, "[preview] timed out waiting for xdg_surface configure\n");
                return false;
            }
        }
        std::printf("[preview] xdg_surface configured (%dx%d)\n", width_, height_);

        if (agl_shell_name_) {
            agl_shell_.Get()->Ready();
            std::printf("[preview] agl_shell.ready sent\n");
        }
        return true;
    }

    bool init_egl()
    {
        auto *wl_surf = reinterpret_cast<wl_surface *>(surface_.Get()->GetProxy());
        if (!setup_egl_wayland(egl_, display_.Get(), wl_surf, width_, height_, &egl_window_))
            return false;
        if (!check_extensions(egl_)) {
            std::fprintf(stderr, "[preview] required EGL extensions missing\n");
            return false;
        }
        return true;
    }

    bool init_gl()
    {
        quad_prog_ = build_program(kQuadVS, kQuadFS);
        if (!quad_prog_) return false;
        quad_u_tex_   = glGetUniformLocation(quad_prog_, "uTex");
        quad_u_scale_ = glGetUniformLocation(quad_prog_, "uScale");
        return true;
    }

    bool init(int w, int h)
    {
        width_ = w; height_ = h;
        if (!connect_display()) return false;
        if (!scan_globals())    return false;
        if (!bind_globals())    return false;
        // setup_shell() must run before init_egl() so xdg_toplevel::configure
        // can update width_/height_ before the EGL window is sized.
        if (!setup_shell())     return false;
        if (!init_egl())        return false;
        if (!init_gl())         return false;
        return true;
    }

    void pump_events() noexcept
    {
        if (display_.IsNull()) return;
        wl_display *dpy = display_.Get();

        // Flush queued outgoing requests; if the socket buffer is briefly
        // full, give the compositor one non-blocking chance to drain it.
        while (wl_display_flush(dpy) < 0 && errno == EAGAIN) {
            pollfd pfd{wl_display_get_fd(dpy), POLLOUT, 0};
            if (poll(&pfd, 1, 0) <= 0) break;
        }

        // Dispatch whatever's already buffered client-side.
        wl_display_dispatch_pending(dpy);

        // Non-blocking check for NEW data on the socket. dispatch_pending()
        // above never reads the fd, so without this, events the compositor
        // sends after start-up — most importantly wl_buffer.release, which
        // wl_egl_window needs to recycle its small back-buffer pool — are
        // never picked up. Left this way, eglSwapBuffers() eventually has no
        // free buffer to draw into once that pool is exhausted, and the
        // window silently stops updating after the first couple of frames
        // even though the render loop keeps running.
        if (wl_display_prepare_read(dpy) == 0) {
            pollfd pfd{wl_display_get_fd(dpy), POLLIN, 0};
            if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                wl_display_read_events(dpy);
                wl_display_dispatch_pending(dpy);
            } else {
                wl_display_cancel_read(dpy);
            }
        }
    }

    void present(GLuint scene_tex, int scene_w, int scene_h) noexcept
    {
        // Letterbox/pillarbox: shrink whichever axis needs it so the source
        // image's aspect ratio is preserved inside the window, instead of
        // stretching it independently on each axis to fill the screen.
        float sx = 1.0f, sy = 1.0f;
        if (scene_w > 0 && scene_h > 0 && width_ > 0 && height_ > 0) {
            float tex_aspect = (float)scene_w / (float)scene_h;
            float win_aspect = (float)width_   / (float)height_;
            if (tex_aspect > win_aspect) sy = win_aspect / tex_aspect;
            else                          sx = tex_aspect / win_aspect;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width_, height_);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(quad_prog_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, scene_tex);
        glUniform1i(quad_u_tex_, 0);
        glUniform2f(quad_u_scale_, sx, sy);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        presentation_.Arm(surface_.Get()->GetProxy(), commit_frame_++);
        eglSwapBuffers(egl_.dpy, egl_.surface);
    }

    void cleanup() noexcept
    {
        if (quad_prog_) { glDeleteProgram(quad_prog_); quad_prog_ = 0; }

        // EGL surface/context before the wl_egl_window it wraps, before the
        // wl_surface/xdg/agl_shell objects, before the registry/display.
        teardown_egl(egl_);
        if (egl_window_) { wl_egl_window_destroy(egl_window_); egl_window_ = nullptr; }

        presentation_.Release();
        xdg_toplevel_.Reset();
        xdg_surface_.Reset();
        agl_shell_.Reset();
        xdg_wm_base_.Reset();
        surface_.Reset();
        output_.Reset();
        compositor_.Reset();
        registry_.Reset();
        display_.Disconnect();
    }

    ~Impl() { cleanup(); }

    // ── Members ────────────────────────────────────────────────────────────
    wl::DisplayHandle display_;
    wl::CRegistry      registry_;

    wl::WlPtr<WlCompositorHandler> compositor_;
    wl::WlPtr<WlSurfaceHandler>    surface_;
    wl::WlPtr<WlOutputHandler>     output_;

    wl::WlPtr<wl::XdgWmBaseHandler>            xdg_wm_base_;
    wl::WlPtr<wl::XdgSurfaceHandler<Impl>>     xdg_surface_;
    wl::WlPtr<wl::XdgToplevelHandler<Impl>>    xdg_toplevel_;
    wl::WlPtr<wl::AglShellHandler<Impl>>       agl_shell_;
    wl::PresentationManager<Impl>              presentation_;

    EGLState        egl_{};
    wl_egl_window  *egl_window_ = nullptr;

    GLuint quad_prog_  = 0;
    GLint  quad_u_tex_   = -1;
    GLint  quad_u_scale_ = -1;

    int width_ = 0, height_ = 0;
    uint32_t commit_frame_ = 0;
    bool running_    = true;
    bool configured_ = false;

    enum class BoundState { Waiting, Ok, Fail };
    BoundState bound_state_ = BoundState::Waiting;

    uint32_t compositor_name_ = 0, compositor_ver_ = 0;
    uint32_t output_name_ = 0, output_ver_ = 0;
    uint32_t xdg_wm_base_name_ = 0, xdg_wm_base_ver_ = 0;
    uint32_t agl_shell_name_ = 0, agl_shell_ver_ = 0;

    static constexpr uint32_t kReportEveryFrames = 120;  // ~2 s at 60 Hz
    uint32_t presented_count_  = 0;
    double   latency_sum_ms_   = 0.0;
    uint32_t last_refresh_ns_  = 0;
};

// ══════════════════════════════════════════════════════════════════════════
// WaylandPreviewWindow — thin forwarding wrapper
// ══════════════════════════════════════════════════════════════════════════

WaylandPreviewWindow::WaylandPreviewWindow() : impl_(std::make_unique<Impl>()) {}
WaylandPreviewWindow::~WaylandPreviewWindow() = default;

bool WaylandPreviewWindow::init(int width, int height) { return impl_->init(width, height); }
void WaylandPreviewWindow::pump_events() { impl_->pump_events(); }
void WaylandPreviewWindow::present(unsigned int scene_tex, int scene_w, int scene_h) {
    impl_->present((GLuint)scene_tex, scene_w, scene_h);
}
bool WaylandPreviewWindow::running() const { return impl_->running_; }
const EGLState &WaylandPreviewWindow::egl_state() const { return impl_->egl_; }
void WaylandPreviewWindow::cleanup() { impl_->cleanup(); }
