#include "pipeline_runtime.h"
#include "bev_config.h"
#include "ipm.h"

bool pipeline_setup(PipelineState& state, const EGLState& eg, const std::string& bev_config_path){
    state.cm = std::make_unique<libcamera::CameraManager>();
    if(state.cm->start()){
        return false;
    }

    if(state.cm->cameras().empty()){
        return false;
    }
    int n = std::min((int)state.cm->cameras().size(), GpuRenderer::kMaxCameras);
    for (size_t i = 0; i < n; i++)
    {
        CameraSlot slot;
        if(!state.cm->cameras().at(i)){
            printf("Error - empty camera place ! %zu\n", i);
        }
        slot.camera = state.cm->cameras().at(i);
        if(slot.camera->acquire()){
            printf("ERROR - camera acquire - %zu\n", i);
        }
        slot.config = slot.camera->generateConfiguration({libcamera::StreamRole::Viewfinder});
                libcamera::StreamConfiguration& sc = slot.config->at(0);
        sc.size        = {1640, 1232};
        sc.pixelFormat = libcamera::formats::NV12;

        slot.config->at(0).pixelFormat = libcamera::formats::NV12;
        slot.config->at(0).size = {1640, 1232};

        if(slot.config->validate() != libcamera::CameraConfiguration::Status::Valid){
            printf("ERRPR - slot.config->validate() - %zu\n", i);
        }
        if(slot.camera->configure(slot.config.get())){
            printf("ERRPR - slot.camera->configure(slot.config.get()) - %zu\n", i);
        }

        slot.stream = slot.config->at(0).stream();
        slot.alloc = std::make_unique<libcamera::FrameBufferAllocator>(slot.camera);
        if(slot.alloc->allocate(slot.stream)) printf("Error - slot.alloc->allocate(slot.stream) - %zu\n", i);

        for(auto &buf : slot.alloc->buffers(slot.stream)){
            auto req = slot.camera->createRequest();
            if(! req || req->addBuffer(slot.stream, buf.get())) return false;
            slot.requests.push_back(std::move(req));
        }

        slot.session = std::make_unique<CaptureSession>(sc, slot.camera.get());
        slot.camera->requestCompleted.connect(slot.session.get(), &CaptureSession::requestCompleted);

        state.slots.push_back(std::move(slot));
    }

    if(state.slots.size() == 1){
        auto &s0 = state.slots[0]; 
        libcamera::StreamConfiguration& sc = s0.config->at(0);
        if(!state.renderer.init(eg, sc, s0.alloc->buffers(s0.stream))) return false;
    }else{
        BevConfig &cfg = state.cfg;
        if(!bev_config_load(bev_config_path, cfg)) return false;

        libcamera::StreamConfiguration& sc0 = state.slots[0].config->at(0);
        int cw = sc0.size.width, ch = sc0.size.height;
        state.canvas_w = cw; state.canvas_h = ch;
        if(!state.renderer.init_multi_live(eg, cw, ch, sc0.stride, (int)state.slots.size())) return false;

        state.renderer.set_stitch_overlap(cfg.overlap_deg);
        state.renderer.set_blend_edge(cfg.blend_edge);
        state.renderer.set_px_per_m((float)cfg.px_per_m);

        for(size_t i = 0; i < state.slots.size(); ++i){
            const BevSlotConfig &sc_i = cfg.slots[i];
            if(!sc_i.has_hb2i){
                printf("ERROR - bev_config slot %zu has no hb2i, calibrate it first\n", i);
                return false;
            }
            mat3 H = measured_H(sc_i.hb2i, cw, ch, cfg.px_per_m, cw, ch,
                                sc_i.cam_x_delta, sc_i.cam_y_delta, sc_i.yaw_delta);
            float Hf[9];
            H.to_floats(Hf);
            state.renderer.set_ipm_multi((int)i, Hf, (float)sc_i.facing_deg);
        }
    }

    for (auto &slot : state.slots) {
        if (slot.camera->start()) return false;
        for (auto &req : slot.requests)
            slot.camera->queueRequest(req.get());
    }
    for (auto &slot : state.slots)
        slot.session->waitWarmupDone();

    return true;

}


void pipeline_loop(PipelineState& state, std::atomic<bool>& running,
                   std::function<void()> before_render, std::function<void()> after_render){
    while(running.load()){
        std::vector<const libcamera::FrameBuffer*> bufs;
        std::vector<libcamera::Request*> reqs;
        bool stopped = false;
        for(auto &slot : state.slots){
            auto [buf, req] = slot.session->nextFrame();
            if(!buf){ stopped = true; break; }
            bufs.push_back(buf);
            reqs.push_back(req);
        }
        if(stopped) break;

        if(before_render) before_render();
        if(state.slots.size() == 1) state.renderer.render_frame(bufs[0]);
        else                        state.renderer.render_frame_multi_live(bufs);
        if(after_render) after_render();

        for(size_t i = 0; i < reqs.size(); ++i){
            reqs[i]->reuse(libcamera::Request::ReuseBuffers);
            state.slots[i].camera->queueRequest(reqs[i]);
        }
    }
}

void pipeline_teardown(PipelineState& state, EGLState& egl){
    for(auto &slot : state.slots) slot.session->stop();
    for(auto &slot : state.slots) slot.camera->stop();
    state.renderer.cleanup();
    for(auto &slot : state.slots){
        slot.alloc->free(slot.stream);
        slot.camera->release();
    }
    state.cm->stop();
    teardown_egl(egl);
}

void pipeline_set_slot_pose(PipelineState& state, int slot, double dx, double dy, double dyaw_deg){
    if(slot < 0 || slot >= (int)state.slots.size() || state.slots.size() < 2) return;
    const BevSlotConfig &sc = state.cfg.slots[slot];
    if(!sc.has_hb2i) return;
    mat3 H = measured_H(sc.hb2i, state.canvas_w, state.canvas_h, state.cfg.px_per_m,
                        state.canvas_w, state.canvas_h, dx, dy, dyaw_deg);
    float Hf[9];
    H.to_floats(Hf);
    state.renderer.set_ipm_multi(slot, Hf, (float)sc.facing_deg);
}
