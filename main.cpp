#include "egl_context.h"
#include "capture_session.h"

#include <libcamera/libcamera.h>

#include <iostream>
#include <memory>

using namespace libcamera;

int main()
{
    /* ---- EGL/GBM headless context ---- */
    EGLState egl;
    if (!setup_egl(egl)) {
        teardown_egl(egl);
        return 1;
    }
    if (!check_extensions(egl)) {
        std::cerr << "[egl] one or more required extensions missing — "
                     "cannot proceed to DMA-BUF import\n";
        teardown_egl(egl);
        return 1;
    }
    std::cout << "[egl] context ready\n\n";

    /* ---- libcamera capture ---- */
    auto cm = std::make_unique<CameraManager>();
    int r = cm->start();
    if (r) {
        std::cerr << "[capture] CameraManager::start failed: " << r << "\n";
        teardown_egl(egl);
        return 1;
    }
    if (cm->cameras().empty()) {
        std::cerr << "[capture] no cameras detected\n";
        cm->stop();
        teardown_egl(egl);
        return 1;
    }

    std::shared_ptr<Camera> camera = cm->cameras()[0];
    std::cout << "[capture] camera: " << camera->id() << "\n";

    r = camera->acquire();
    if (r) {
        std::cerr << "[capture] Camera::acquire failed: " << r << "\n";
        cm->stop();
        teardown_egl(egl);
        return 1;
    }

    auto config = camera->generateConfiguration({StreamRole::Viewfinder});
    if (!config) {
        camera->release(); cm->stop(); teardown_egl(egl); return 1;
    }

    StreamConfiguration &sc = config->at(0);
    sc.size        = {1640, 1232};
    sc.pixelFormat = formats::NV12;

    CameraConfiguration::Status st = config->validate();
    if (st == CameraConfiguration::Invalid) {
        std::cout << "[capture] NV12 not supported, trying YUYV\n";
        sc.pixelFormat = formats::YUYV;
        st = config->validate();
    }
    if (st == CameraConfiguration::Invalid) {
        std::cerr << "[capture] no supported format\n";
        camera->release(); cm->stop(); teardown_egl(egl); return 1;
    }
    if (st == CameraConfiguration::Adjusted)
        std::cout << "[capture] config adjusted: " << sc.toString() << "\n";

    r = camera->configure(config.get());
    if (r) {
        std::cerr << "[capture] Camera::configure failed: " << r << "\n";
        camera->release(); cm->stop(); teardown_egl(egl); return 1;
    }

    Stream *stream = sc.stream();
    FrameBufferAllocator alloc(camera);
    if (alloc.allocate(stream) < 0) {
        camera->release(); cm->stop(); teardown_egl(egl); return 1;
    }

    auto request = camera->createRequest();
    if (!request ||
        request->addBuffer(stream, alloc.buffers(stream)[0].get())) {
        camera->release(); cm->stop(); teardown_egl(egl); return 1;
    }

    CaptureSession session(sc, camera.get());
    camera->requestCompleted.connect(&session,
                                     &CaptureSession::requestCompleted);
    std::cout << "[capture] warming up AE/AWB ("
              << CaptureSession::WARMUP_FRAMES << " frames)...\n";

    r = camera->start();
    if (r) {
        camera->release(); cm->stop(); teardown_egl(egl); return 1;
    }
    r = camera->queueRequest(request.get());
    if (r) {
        camera->stop(); camera->release();
        cm->stop(); teardown_egl(egl); return 1;
    }

    session.waitDone();

    /* ---- Cleanup ---- */
    camera->stop();
    alloc.free(stream);
    camera->release();
    cm->stop();

    teardown_egl(egl);
    std::cout << "[exit] clean\n";
    return 0;
}
