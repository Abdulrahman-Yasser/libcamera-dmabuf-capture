#pragma once

#include <libcamera/libcamera.h>

#include <condition_variable>
#include <fstream>
#include <mutex>

class CaptureSession {
public:
    static constexpr int WARMUP_FRAMES = 30;

    explicit CaptureSession(const libcamera::StreamConfiguration &sc,
                            libcamera::Camera *camera);

    void requestCompleted(libcamera::Request *req);
    void waitDone();

private:
    void saveRaw(const libcamera::FrameBuffer *buf);
    void writeRows(std::ofstream &out,
                   const libcamera::FrameBuffer::Plane &plane,
                   unsigned int rows);

    const libcamera::StreamConfiguration &sc_;
    libcamera::Camera                    *camera_;
    std::mutex                            mtx_;
    std::condition_variable               cv_;
    bool                                  done_       = false;
    int                                   frameCount_ = 0;
};
