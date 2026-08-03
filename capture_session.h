#pragma once

#include <libcamera/libcamera.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <utility>

class CaptureSession {
public:
    static constexpr int WARMUP_FRAMES = 30;

    CaptureSession(const libcamera::StreamConfiguration &sc,
                   libcamera::Camera *camera);

    void requestCompleted(libcamera::Request *req);

    // Block until AE/AWB warmup frames are done.
    void waitWarmupDone();

    // Block until the next frame is available.
    // Returns {buffer, request} — caller must requeue the request.
    // Returns {nullptr, nullptr} after stop() is called.
    std::pair<const libcamera::FrameBuffer *,
              libcamera::Request *> nextFrame();

    void stop();

private:
    const libcamera::StreamConfiguration &sc_;
    libcamera::Camera                    *camera_;
    int                                   frameCount_ = 0;

    std::mutex              warmup_mtx_;
    std::condition_variable warmup_cv_;
    bool                    warmup_done_ = false;

    std::mutex              mtx_;
    std::condition_variable cv_;
    std::queue<std::pair<const libcamera::FrameBuffer *,
                         libcamera::Request *>> ready_;
    std::atomic<bool>       running_{true};
};
