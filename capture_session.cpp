#include "capture_session.h"

#include <sys/mman.h>

#include <iostream>

using namespace libcamera;

CaptureSession::CaptureSession(const StreamConfiguration &sc, Camera *camera)
    : sc_(sc), camera_(camera) {}

void CaptureSession::requestCompleted(Request *req)
{
    if (req->status() == Request::RequestCancelled)
        return;

    ++frameCount_;

    if (frameCount_ <= WARMUP_FRAMES) {
        req->reuse(Request::ReuseBuffers);
        camera_->queueRequest(req);
        return;
    }

    const FrameBuffer *buf    = req->buffers().begin()->second;
    const auto        &planes = buf->planes();

    std::cout << "=== Frame captured (frame " << frameCount_ << ") ===\n"
              << "  Width      : " << sc_.size.width  << "\n"
              << "  Height     : " << sc_.size.height << "\n"
              << "  Stride     : " << sc_.stride      << "\n"
              << "  PixelFormat: " << sc_.pixelFormat.toString() << "\n"
              << "  Planes     : " << planes.size() << "\n";

    for (size_t i = 0; i < planes.size(); ++i)
        std::cout << "  Plane[" << i << "]:"
                  << "  fd="     << planes[i].fd.get()
                  << "  offset=" << planes[i].offset
                  << "  length=" << planes[i].length << "\n";

    saveRaw(buf);

    {
        std::lock_guard<std::mutex> lk(mtx_);
        done_ = true;
    }
    cv_.notify_one();
}

void CaptureSession::waitDone()
{
    std::unique_lock<std::mutex> lk(mtx_);
    cv_.wait(lk, [this] { return done_; });
}

void CaptureSession::saveRaw(const FrameBuffer *buf)
{
    std::ofstream out("/tmp/frame.raw",
                      std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "[capture] cannot open /tmp/frame.raw\n";
        return;
    }

    const auto        &planes = buf->planes();
    const unsigned int w      = sc_.size.width;
    const unsigned int h      = sc_.size.height;

    writeRows(out, planes[0], h);        /* Y  plane */
    writeRows(out, planes[1], h / 2);    /* UV plane */

    std::cout << "[capture] /tmp/frame.raw saved ("
              << w << "x" << h << " NV12, stride=" << sc_.stride << ")\n";
}

void CaptureSession::writeRows(std::ofstream &out,
                               const FrameBuffer::Plane &plane,
                               unsigned int rows)
{
    const unsigned int w      = sc_.size.width;
    const unsigned int stride = sc_.stride;
    size_t total = plane.offset + plane.length;
    void  *mem   = mmap(nullptr, total, PROT_READ, MAP_SHARED,
                        plane.fd.get(), 0);
    if (mem == MAP_FAILED) { perror("[capture] mmap"); return; }
    const uint8_t *src = static_cast<const uint8_t *>(mem) + plane.offset;
    for (unsigned int row = 0; row < rows; ++row)
        out.write(reinterpret_cast<const char *>(src + row * stride), w);
    munmap(mem, total);
}
