#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <libcamera/libcamera.h>
#include "egl_context.h"
#include "gpu_renderer.h"

#include "capture_session.h"
#include "bev_config.h"

struct CameraSlot{
    std::shared_ptr<libcamera::Camera>                  camera;
    std::unique_ptr<libcamera::CameraConfiguration>     config;
    libcamera::Stream*                                  stream;
    std::unique_ptr<libcamera::FrameBufferAllocator>    alloc;
    std::vector<std::unique_ptr<libcamera::Request>>    requests;
    std::unique_ptr<CaptureSession>                     session;
};

struct PipelineState{
    std::unique_ptr<libcamera::CameraManager>   cm;
    std::vector<CameraSlot>                     slots;
    GpuRenderer                                         renderer;
    BevConfig                                   cfg;
    int                                         canvas_w = 0;
    int                                         canvas_h = 0;
};

bool pipeline_setup(PipelineState& state, const EGLState& eg,
                    const std::string& bev_config_path = "bev_config.ini");
void pipeline_loop(PipelineState& state, std::atomic<bool>& running,
                   std::function<void()> before_render = nullptr,
                   std::function<void()> after_render = nullptr);
void pipeline_set_slot_pose(PipelineState& state, int slot, double dx, double dy, double dyaw_deg);
void pipeline_teardown(PipelineState& state, EGLState& egl);
