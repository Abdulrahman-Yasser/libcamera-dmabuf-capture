#include <libcamera/libcamera.h>

#include <sys/mman.h>
#include <fcntl.h>

#include <condition_variable>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>

using namespace libcamera;

static constexpr int WARMUP_FRAMES = 30;

class CaptureSession {
public:
    explicit CaptureSession(const StreamConfiguration &sc, Camera *camera,
                            const Rectangle &cropRect)
        : sc_(sc), camera_(camera), cropRect_(cropRect) {}

    void requestCompleted(Request *req)
    {
        if (req->status() == Request::RequestCancelled)
            return;

        ++frameCount_;

        if (frameCount_ <= WARMUP_FRAMES) {
            req->reuse(Request::ReuseBuffers);
            if (cropRect_.width > 0)
                req->controls().set(controls::ScalerCrop, cropRect_);
            camera_->queueRequest(req);
            return;
        }

        const FrameBuffer *buf = req->buffers().begin()->second;
        const auto &planes = buf->planes();

        std::cout << "=== Frame captured (frame " << frameCount_ << ") ===\n"
                  << "  Width      : " << sc_.size.width  << "\n"
                  << "  Height     : " << sc_.size.height << "\n"
                  << "  Stride     : " << sc_.stride      << "\n"
                  << "  PixelFormat: " << sc_.pixelFormat.toString() << "\n"
                  << "  Planes     : " << planes.size() << "\n";

        for (size_t i = 0; i < planes.size(); ++i)
            std::cout << "  Plane[" << i << "]:"
                      << "  fd="     << planes[i].fd.get()
                      << "  offset=" << planes[i].offset
                      << "  length=" << planes[i].length << "\n";

        saveRaw(buf);

        {
            std::lock_guard<std::mutex> lk(mtx_);
            done_ = true;
        }
        cv_.notify_one();
    }

    void waitDone()
    {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return done_; });
    }

private:
    void saveRaw(const FrameBuffer *buf)
    {
        std::ofstream out("/tmp/frame.raw", std::ios::binary | std::ios::trunc);
        if (!out) { std::cerr << "[capture] cannot open /tmp/frame.raw\n"; return; }
        for (const FrameBuffer::Plane &plane : buf->planes()) {
            size_t total = plane.offset + plane.length;
            void  *mem   = mmap(nullptr, total, PROT_READ, MAP_SHARED,
                                plane.fd.get(), 0);
            if (mem == MAP_FAILED) { perror("[capture] mmap"); continue; }
            out.write(static_cast<const char *>(mem) + plane.offset,
                      plane.length);
            munmap(mem, total);
        }
        std::cout << "[capture] raw frame saved to /tmp/frame.raw\n";
    }

    const StreamConfiguration &sc_;
    Camera                    *camera_;
    Rectangle                  cropRect_;
    std::mutex                 mtx_;
    std::condition_variable    cv_;
    bool                       done_       = false;
    int                        frameCount_ = 0;
};

int main()
{
    auto cm = std::make_unique<CameraManager>();
    if (int r = cm->start(); r) {
        std::cerr << "[capture] CameraManager::start failed: " << r << "\n";
        return 1;
    }
    if (cm->cameras().empty()) {
        std::cerr << "[capture] no cameras detected\n";
        cm->stop(); return 1;
    }

    std::shared_ptr<Camera> camera = cm->cameras()[0];
    std::cout << "[capture] camera: " << camera->id() << "\n";

    if (int r = camera->acquire(); r) {
        std::cerr << "[capture] Camera::acquire failed: " << r << "\n";
        cm->stop(); return 1;
    }

    auto config = camera->generateConfiguration({StreamRole::Viewfinder});
    if (!config) {
        camera->release(); cm->stop(); return 1;
    }

    StreamConfiguration &sc = config->at(0);
    sc.size        = {1280, 720};
    sc.pixelFormat = formats::NV12;

    CameraConfiguration::Status st = config->validate();
    if (st == CameraConfiguration::Invalid) {
        std::cout << "[capture] NV12 not supported, trying YUYV\n";
        sc.pixelFormat = formats::YUYV;
        st = config->validate();
    }
    if (st == CameraConfiguration::Invalid) {
        std::cerr << "[capture] no supported format\n";
        camera->release(); cm->stop(); return 1;
    }
    if (st == CameraConfiguration::Adjusted)
        std::cout << "[capture] config adjusted to: " << sc.toString() << "\n";

    if (int r = camera->configure(config.get()); r) {
        std::cerr << "[capture] Camera::configure failed: " << r << "\n";
        camera->release(); cm->stop(); return 1;
    }

    Stream *stream = sc.stream();

    Rectangle sensorCrop;
    {
        const auto &props = camera->properties();
        auto activeAreas = props.get(properties::PixelArrayActiveAreas);
        if (activeAreas && !activeAreas->empty()) {
            sensorCrop = (*activeAreas)[0];
            std::cout << "[capture] sensor active area: " << sensorCrop.toString() << "\n";
        } else {
            auto sz = props.get(properties::PixelArraySize);
            if (sz) {
                sensorCrop = Rectangle(0, 0, sz->width, sz->height);
                std::cout << "[capture] sensor pixel array: " << sensorCrop.toString() << "\n";
            }
        }
    }

    FrameBufferAllocator alloc(camera);
    if (alloc.allocate(stream) < 0) {
        camera->release(); cm->stop(); return 1;
    }

    auto request = camera->createRequest();
    if (!request || request->addBuffer(stream, alloc.buffers(stream)[0].get())) {
        camera->release(); cm->stop(); return 1;
    }
    if (sensorCrop.width > 0)
        request->controls().set(controls::ScalerCrop, sensorCrop);

    CaptureSession session(sc, camera.get(), sensorCrop);
    camera->requestCompleted.connect(&session, &CaptureSession::requestCompleted);
    std::cout << "[capture] warming up AE/AWB (" << WARMUP_FRAMES << " frames)...\n";

    if (int r = camera->start(); r) {
        camera->release(); cm->stop(); return 1;
    }
    if (int r = camera->queueRequest(request.get()); r) {
        camera->stop(); camera->release(); cm->stop(); return 1;
    }

    session.waitDone();

    camera->stop();
    alloc.free(stream);
    camera->release();
    cm->stop();

    return 0;
}
