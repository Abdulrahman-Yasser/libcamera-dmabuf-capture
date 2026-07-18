#include "file_source.h"

#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include <cstdio>
#include <iostream>

// This board's SoC (RPi5/BCM2712) has no H.264 hardware decode block — only
// HEVC ("rpi-hevc-dec"). Its hardware decoder (v4l2h265dec / v4l2slh265dec)
// only produces Broadcom's proprietary tiled NV12 variant ("NC12", 128-byte
// column tiles), which GStreamer's v4l2codecs plugin can't express as
// negotiable caps at all (confirmed via v4l2-ctl --list-formats: capture
// side only offers NC12/NC30, no plain linear NV12). De-tiling that would
// normally go through the board's separate "pispbe" ISP block via a
// v4l2convert element, but this GStreamer build has no v4l2 transform
// element registered at all (checked: gst-inspect-1.0 | grep v4l2 shows
// only the decoder + v4l2src/sink/radio). So: software decode via libav's
// avdec_h265 instead — it outputs standard planar video directly, sidestepping
// the tiled-memory problem entirely. Slower than hardware decode, but this
// is file-mode/debug-only code, not the camera hot path.
static GstElement *make_hevc_decoder()
{
    GstElement *dec = gst_element_factory_make("avdec_h265", "dec");
    if (dec) std::printf("[file] HEVC decoder: avdec_h265 (software)\n");
    return dec;
}

// avdec_h265 outputs standard planar YUV (typically I420), not NV12 —
// videoconvert bridges that to the NV12 appsink expects. This is a normal,
// fully-supported conversion (unlike the tiled-format dead end above).
static GstElement *make_converter()
{
    GstElement *conv = gst_element_factory_make("videoconvert", "conv");
    if (conv) std::printf("[file] converter: videoconvert\n");
    return conv;
}

// Pop and print any pending ERROR/WARNING messages from the pipeline bus.
// Without this, a caps-negotiation failure (e.g. the decoder only offering
// DMABuf-memory output while appsink asked for plain system-memory NV12)
// shows up as a silent EOS/timeout with no indication of the real cause.
static void drain_bus_errors(GstElement *pipeline, const char *ctx)
{
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg;
    while ((msg = gst_bus_pop_filtered(
                bus, (GstMessageType)(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING)))) {
        GError *err = nullptr;
        gchar  *dbg = nullptr;
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            gst_message_parse_error(msg, &err, &dbg);
            std::cerr << "[file] " << ctx << " ERROR: " << err->message << "\n";
        } else {
            gst_message_parse_warning(msg, &err, &dbg);
            std::cerr << "[file] " << ctx << " WARNING: " << err->message << "\n";
        }
        if (dbg) std::cerr << "[file]   debug: " << dbg << "\n";
        g_error_free(err);
        g_free(dbg);
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
}

bool FileSource::open(const std::string &path)
{
    static bool gst_inited = false;
    if (!gst_inited) {
        gst_init(nullptr, nullptr);
        gst_inited = true;
    }

    GstElement *src   = gst_element_factory_make("filesrc",   "src");
    GstElement *parse = gst_element_factory_make("h265parse", "parse");
    GstElement *dec   = make_hevc_decoder();
    GstElement *conv  = make_converter();
    GstElement *sink  = gst_element_factory_make("appsink",   "sink");

    if (!src || !parse || !dec || !conv || !sink) {
        if (!src)   std::cerr << "[file] missing element: filesrc\n";
        if (!parse) std::cerr << "[file] missing element: h265parse (gstreamer1.0-plugins-bad)\n";
        if (!dec)   std::cerr << "[file] missing element: avdec_h265 "
                                 "(gstreamer1.0-libav)\n";
        if (!conv)  std::cerr << "[file] missing element: videoconvert "
                                 "(gstreamer1.0-plugins-base)\n";
        if (!sink)  std::cerr << "[file] missing element: appsink (gstreamer1.0-plugins-base)\n";
        if (src)   gst_object_unref(src);
        if (parse) gst_object_unref(parse);
        if (dec)   gst_object_unref(dec);
        if (conv)  gst_object_unref(conv);
        if (sink)  gst_object_unref(sink);
        return false;
    }

    g_object_set(src, "location", path.c_str(), nullptr);
    g_object_set(sink, "sync", FALSE, "max-buffers", (guint)4, "drop", FALSE, nullptr);
    gst_app_sink_set_emit_signals(GST_APP_SINK(sink), FALSE);

    // Restrict appsink to plain system-memory NV12 (no memory:DMABuf feature)
    // so we can CPU-map and upload via glTexImage2D. The decoder itself only
    // produces DMABuf-memory buffers (RPi5's stateless v4l2sl*dec); the
    // converter element bridges that to system memory before it reaches here.
    GstCaps *caps = gst_caps_from_string("video/x-raw, format=(string)NV12");
    gst_app_sink_set_caps(GST_APP_SINK(sink), caps);
    gst_caps_unref(caps);

    pipeline_ = gst_pipeline_new("file-src");
    appsink_  = sink;

    gst_bin_add_many(GST_BIN(pipeline_), src, parse, dec, conv, sink, nullptr);
    if (!gst_element_link_many(src, parse, dec, conv, sink, nullptr)) {
        std::cerr << "[file] failed to link pipeline elements\n";
        gst_object_unref(pipeline_);
        pipeline_ = appsink_ = nullptr;
        return false;
    }

    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[file] pipeline failed to start\n";
        drain_bus_errors(pipeline_, path.c_str());
        gst_object_unref(pipeline_);
        pipeline_ = appsink_ = nullptr;
        return false;
    }

    // Block until the pipeline actually reaches PLAYING (or fails) so a
    // caps-negotiation error surfaces here, not as a mysterious later EOS.
    GstState state;
    GstStateChangeReturn ret =
        gst_element_get_state(pipeline_, &state, nullptr, 3 * GST_SECOND);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        std::cerr << "[file] pipeline failed to reach PLAYING\n";
        drain_bus_errors(pipeline_, path.c_str());
        gst_object_unref(pipeline_);
        pipeline_ = appsink_ = nullptr;
        return false;
    }
    drain_bus_errors(pipeline_, path.c_str()); // print any non-fatal warnings too

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
        drain_bus_errors(pipeline_, "nextFrame");
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

    // One-time framerate probe (vmeta doesn't carry it, so this runs
    // independently of which layout branch was taken above).
    if (!fps_probed_) {
        fps_probed_ = true;
        GstCaps *scaps = gst_sample_get_caps(last_sample_);
        GstVideoInfo vi;
        if (scaps && gst_video_info_from_caps(&vi, scaps)) {
            fps_n_ = GST_VIDEO_INFO_FPS_N(&vi);
            fps_d_ = GST_VIDEO_INFO_FPS_D(&vi);
            if (fps_n_ > 0)
                std::printf("[file] framerate: %d/%d (%.2f ms/frame)\n",
                           fps_n_, fps_d_, frame_duration_ms());
        }
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
    fps_n_ = fps_d_ = 0;
    fps_probed_ = false;
}
