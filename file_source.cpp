#include "file_source.h"

#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include <cstdio>
#include <iostream>

bool FileSource::open(const std::string &path)
{
    static bool gst_inited = false;
    if (!gst_inited) {
        gst_init(nullptr, nullptr);
        gst_inited = true;
    }

    GstElement *src   = gst_element_factory_make("filesrc",     "src");
    GstElement *parse = gst_element_factory_make("h264parse",   "parse");
    GstElement *dec   = gst_element_factory_make("v4l2h264dec", "dec");
    GstElement *sink  = gst_element_factory_make("appsink",     "sink");

    if (!src || !parse || !dec || !sink) {
        if (!src)   std::cerr << "[file] missing element: filesrc\n";
        if (!parse) std::cerr << "[file] missing element: h264parse (gstreamer1.0-plugins-bad)\n";
        if (!dec)   std::cerr << "[file] missing element: v4l2h264dec (gstreamer1.0-plugins-bad)\n";
        if (!sink)  std::cerr << "[file] missing element: appsink (gstreamer1.0-plugins-base)\n";
        if (src)   gst_object_unref(src);
        if (parse) gst_object_unref(parse);
        if (dec)   gst_object_unref(dec);
        if (sink)  gst_object_unref(sink);
        return false;
    }

    g_object_set(src, "location", path.c_str(), nullptr);
    g_object_set(sink, "sync", FALSE, "max-buffers", (guint)4, "drop", FALSE, nullptr);
    gst_app_sink_set_emit_signals(GST_APP_SINK(sink), FALSE);

    // Restrict to NV12 system-memory output. No memory:DMABuf — bcm2835-codec
    // outputs MMAP buffers, so we CPU-map and upload via glTexImage2D.
    GstCaps *caps = gst_caps_from_string("video/x-raw, format=(string)NV12");
    gst_app_sink_set_caps(GST_APP_SINK(sink), caps);
    gst_caps_unref(caps);

    pipeline_ = gst_pipeline_new("file-src");
    appsink_  = sink;

    gst_bin_add_many(GST_BIN(pipeline_), src, parse, dec, sink, nullptr);
    if (!gst_element_link_many(src, parse, dec, sink, nullptr)) {
        std::cerr << "[file] failed to link pipeline elements\n";
        gst_object_unref(pipeline_);
        pipeline_ = appsink_ = nullptr;
        return false;
    }

    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[file] pipeline failed to start\n";
        gst_object_unref(pipeline_);
        pipeline_ = appsink_ = nullptr;
        return false;
    }

    std::printf("[file] pipeline started: %s\n", path.c_str());
    return true;
}

DmaBufFrame FileSource::nextFrame()
{
    if (mapped_buf_) {
        gst_buffer_unmap(mapped_buf_, &map_info_);
        mapped_buf_ = nullptr;
    }
    if (last_sample_) {
        gst_sample_unref(last_sample_);
        last_sample_ = nullptr;
    }

    GstSample *sample = gst_app_sink_try_pull_sample(
        GST_APP_SINK(appsink_), 5 * GST_SECOND);

    if (!sample) {
        if (gst_app_sink_is_eos(GST_APP_SINK(appsink_)))
            std::printf("[file] EOS\n");
        else
            std::cerr << "[file] pull timeout\n";
        return {};
    }

    GstBuffer *buf = gst_sample_get_buffer(sample);

    if (!gst_buffer_map(buf, &map_info_, GST_MAP_READ)) {
        std::cerr << "[file] failed to map buffer\n";
        gst_sample_unref(sample);
        return {};
    }

    mapped_buf_  = buf;
    last_sample_ = sample;

    DmaBufFrame frame;
    frame.fd   = -1;
    frame.data = static_cast<const uint8_t *>(map_info_.data);

    GstVideoMeta *vmeta = gst_buffer_get_video_meta(buf);
    if (vmeta) {
        frame.width     = (int)vmeta->width;
        frame.height    = (int)vmeta->height;
        frame.stride    = (int)vmeta->stride[0];
        frame.y_offset  = (int)vmeta->offset[0];
        frame.uv_offset = (int)vmeta->offset[1];
        if (w_ == 0) {
            w_ = frame.width; h_ = frame.height; stride_ = frame.stride;
            std::printf("[file] first frame (vmeta): %dx%d stride=%d "
                        "y_off=%d uv_off=%d bufsz=%zu\n",
                        w_, h_, stride_, frame.y_offset, frame.uv_offset,
                        (size_t)map_info_.size);
        }
    } else {
        // GstVideoMeta not attached — derive layout from negotiated caps.
        if (w_ == 0) {
            GstCaps *scaps = gst_sample_get_caps(last_sample_);
            GstVideoInfo vi;
            if (scaps && gst_video_info_from_caps(&vi, scaps)) {
                w_      = GST_VIDEO_INFO_WIDTH(&vi);
                h_      = GST_VIDEO_INFO_HEIGHT(&vi);
                stride_ = GST_VIDEO_INFO_PLANE_STRIDE(&vi, 0);
                uv_off_ = (int)GST_VIDEO_INFO_PLANE_OFFSET(&vi, 1);
                std::printf("[file] first frame (caps): %dx%d stride=%d "
                            "uv_off=%d fmt=%s bufsz=%zu\n",
                            w_, h_, stride_, uv_off_,
                            gst_video_format_to_string(GST_VIDEO_INFO_FORMAT(&vi)),
                            (size_t)map_info_.size);
            } else {
                std::cerr << "[file] cannot determine frame dimensions\n";
            }
        }
        frame.width     = w_;
        frame.height    = h_;
        frame.stride    = stride_;
        frame.y_offset  = 0;
        frame.uv_offset = uv_off_;
    }

    return frame;
}

void FileSource::close()
{
    if (mapped_buf_) {
        gst_buffer_unmap(mapped_buf_, &map_info_);
        mapped_buf_ = nullptr;
    }
    if (last_sample_) { gst_sample_unref(last_sample_); last_sample_ = nullptr; }
    if (pipeline_) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = appsink_ = nullptr;
    }
    w_ = h_ = stride_ = uv_off_ = 0;
}
