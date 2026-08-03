#pragma once

#include "dmabuf_frame.h"
#include <gst/gst.h>
#include <string>
#include <utility>

class FileSource {
public:
    FileSource() = default;
    ~FileSource() { close(); }

    // Not copyable: the implicit shallow copy over raw GstElement*/GstSample*/
    // GstBuffer* pointers would double-free on destruction. Movable instead,
    // so a FileSource can live in a std::vector<Slot>-style container.
    FileSource(const FileSource &) = delete;
    FileSource &operator=(const FileSource &) = delete;
    FileSource(FileSource &&other) noexcept { *this = std::move(other); }
    FileSource &operator=(FileSource &&other) noexcept
    {
        if (this == &other) return *this;
        close();
        pipeline_    = std::exchange(other.pipeline_, nullptr);
        appsink_     = std::exchange(other.appsink_, nullptr);
        last_sample_ = std::exchange(other.last_sample_, nullptr);
        mapped_buf_  = std::exchange(other.mapped_buf_, nullptr);
        map_info_    = std::exchange(other.map_info_, GstMapInfo{});
        w_           = std::exchange(other.w_, 0);
        h_           = std::exchange(other.h_, 0);
        stride_      = std::exchange(other.stride_, 0);
        uv_off_      = std::exchange(other.uv_off_, 0);
        fps_n_       = std::exchange(other.fps_n_, 0);
        fps_d_       = std::exchange(other.fps_d_, 0);
        fps_probed_  = std::exchange(other.fps_probed_, false);
        return *this;
    }

    bool open(const std::string &path);
    void close();

    // Pull the next decoded NV12 frame (system memory).
    // frame.data is valid until the next call to nextFrame() or close().
    // Returns data=nullptr on EOS or error.
    DmaBufFrame nextFrame();

    int width()  const { return w_; }
    int height() const { return h_; }
    int stride() const { return stride_; }

    // Nominal frame duration from the stream's negotiated caps (0.0 if
    // unknown, e.g. before the first frame or a variable-framerate stream).
    // Used by --preview mode to pace playback to the video's real rate —
    // appsink runs with sync=FALSE, so nextFrame() otherwise returns frames
    // as fast as the decoder produces them.
    double frame_duration_ms() const
    {
        return fps_n_ > 0 ? 1000.0 * (double)fps_d_ / (double)fps_n_ : 0.0;
    }

private:
    GstElement *pipeline_    = nullptr;
    GstElement *appsink_     = nullptr;
    GstSample  *last_sample_ = nullptr;
    GstBuffer  *mapped_buf_  = nullptr;
    GstMapInfo  map_info_    = {};
    int w_ = 0, h_ = 0, stride_ = 0, uv_off_ = 0;
    int fps_n_ = 0, fps_d_ = 0;
    bool fps_probed_ = false;
};
