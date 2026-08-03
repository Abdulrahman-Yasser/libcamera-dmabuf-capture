#include "capture_session.h"

#include <iostream>

using namespace libcamera;

CaptureSession::CaptureSession(const StreamConfiguration &sc, Camera *camera)
    : sc_(sc), camera_(camera) {}

void CaptureSession::requestCompleted(Request *req)
{
    if (req->status() == Request::RequestCancelled)
        return;

    ++frameCount_;

    // Warmup phase: requeue silently until AE/AWB has settled.
    if (frameCount_ <= WARMUP_FRAMES) {
        req->reuse(Request::ReuseBuffers);
        camera_->queueRequest(req);
        if (frameCount_ == WARMUP_FRAMES) {
            std::lock_guard<std::mutex> lk(warmup_mtx_);
            warmup_done_ = true;
            warmup_cv_.notify_one();
        }
        return;
    }

    // Post-warmup: push frame to queue for main loop to consume.
    const FrameBuffer *buf = req->buffers().begin()->second;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        ready_.push({buf, req});
    }
    cv_.notify_one();
}

void CaptureSession::waitWarmupDone()
{
    std::unique_lock<std::mutex> lk(warmup_mtx_);
    warmup_cv_.wait(lk, [this] { return warmup_done_; });
    std::cout << "[capture] warmup done — entering render loop\n";
}

std::pair<const FrameBuffer *, Request *> CaptureSession::nextFrame()
{
    std::unique_lock<std::mutex> lk(mtx_);
    cv_.wait(lk, [this] { return !ready_.empty() || !running_; });
    if (ready_.empty())
        return {nullptr, nullptr};
    auto item = ready_.front();
    ready_.pop();
    return item;
}

void CaptureSession::stop()
{
    running_ = false;
    cv_.notify_all();
}
