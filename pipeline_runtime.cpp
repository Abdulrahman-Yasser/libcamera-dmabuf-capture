#include "pipeline_runtime.h"

bool pipeline_setup(PipelineState& state, const EGLState& eg){
    state.cm = std::make_unique<libcamera::CameraManager>();
    if(state.cm->start()){
        return false;
    }

    if(state.cm->cameras().empty()){
        return false;
    }

    state.camera = state.cm->cameras()[0];
    state.camera->acquire();
    state.config = state.camera->generateConfiguration({libcamera::StreamRole::Viewfinder});
    libcamera::StreamConfiguration& sc = state.config->at(0);

    sc.size        = {1640, 1232};
    sc.pixelFormat = libcamera::formats::NV12;

    state.camera->configure(state.config.get());
    state.stream = sc.stream();
    state.alloc = std::make_unique<libcamera::FrameBufferAllocator>(state.camera);
    state.alloc->allocate(state.stream);
    for (auto &buf : state.alloc->buffers(state.stream)){
        auto req = state.camera->createRequest();
        if(!req || req->addBuffer(state.stream, buf.get())){
            return false;
        }
        state.requests.push_back(std::move(req));
    }
    state.renderer.init(eg, sc, state.alloc->buffers(state.stream));
    state.session = std::make_unique<CaptureSession>(sc, state.camera.get());
    state.camera->requestCompleted.connect(state.session.get(), &CaptureSession::requestCompleted);
    state.camera->start();
    for(auto &req : state.requests){
        state.camera->queueRequest(req.get());
    }
    state.session->waitWarmupDone();
    return true;
}


void pipeline_loop(PipelineState& state, std::atomic<bool>& running){
    while(running.load()){
        auto [buf, req] = state.session->nextFrame();
        if(!buf) break;
        state.renderer.render_frame(buf);
        req->reuse(libcamera::Request::ReuseBuffers);
        state.camera->queueRequest(req);
    }
}

void pipeline_teardown(PipelineState& state, EGLState& egl){
    state.session->stop();
    state.camera->stop();
    state.renderer.cleanup();
    state.alloc->free(state.stream);
    state.camera->release();
    state.cm->stop();
    teardown_egl(egl);
}
