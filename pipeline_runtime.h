#pragma once

#include <atomic>
#include <memory>
#include <vector>
#include <libcamera/libcamera.h>
#include "egl_context.h"
#include "gpu_renderer.h"

#include "capture_session.h"

struct PipelineState{
    std::unique_ptr<libcamera::CameraManager>               cm;
    std::shared_ptr<libcamera::Camera>                  camera;
    std::unique_ptr<libcamera::CameraConfiguration>     config;
    libcamera::Stream*                                  stream;
    std::unique_ptr<libcamera::FrameBufferAllocator>    alloc;
    std::vector<std::unique_ptr<libcamera::Request>>    requests;
    GpuRenderer                                         renderer;
    std::unique_ptr<CaptureSession>                     session;
};


bool pipeline_setup(PipelineState& state, const EGLState& eg);
void pipeline_loop(PipelineState& state, std::atomic<bool>& running);
void pipeline_teardown(PipelineState& state, EGLState& egl);
