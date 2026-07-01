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

private:
    GstElement *pipeline_    = nullptr;
    GstElement *appsink_     = nullptr;
    GstSample  *last_sample_ = nullptr;
    GstBuffer  *mapped_buf_  = nullptr;
    GstMapInfo  map_info_    = {};
    int w_ = 0, h_ = 0, stride_ = 0, uv_off_ = 0;
};
