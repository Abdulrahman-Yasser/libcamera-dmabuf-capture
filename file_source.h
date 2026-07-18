#pragma once

#include "dmabuf_frame.h"
#include <gst/gst.h>
#include <string>

class FileSource {
public:
    bool open(const std::string &path);
    void close();
    ~FileSource() { close(); }

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
