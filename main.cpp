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
    explicit CaptureSession(const StreamConfiguration &sc, Camera *camera)
        : sc_(sc), camera_(camera) {}

    void requestCompleted(Request *req)
    {
        if (req->status() == Request::RequestCancelled)
            return;

        ++frameCount_;

        if (frameCount_ <= WARMUP_FRAMES) {
            req->reuse(Request::ReuseBuffers);
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

        const auto        &planes = buf->planes();
        const unsigned int w      = sc_.size.width;
        const unsigned int h      = sc_.size.height;
        const unsigned int stride = sc_.stride;

        /* Strip stride padding: write only w valid bytes per row.
         * The camera aligns rows to 64 bytes (stride >= width), so
         * writing plane.length directly produces a corrupted raw file. */
        auto writeRows = [&](const FrameBuffer::Plane &plane, unsigned int rows) {
            size_t         total = plane.offset + plane.length;
            void          *mem   = mmap(nullptr, total, PROT_READ, MAP_SHARED,
                                        plane.fd.get(), 0);
            if (mem == MAP_FAILED) { perror("[capture] mmap"); return; }
            const uint8_t *src   = static_cast<const uint8_t *>(mem) + plane.offset;
            for (unsigned int row = 0; row < rows; ++row)
                out.write(reinterpret_cast<const char *>(src + row * stride), w);
            munmap(mem, total);
        };

        writeRows(planes[0], h);      /* Y  plane: h rows   */
        writeRows(planes[1], h / 2);  /* UV plane: h/2 rows */

        std::cout << "[capture] raw frame saved to /tmp/frame.raw ("
                  << w << "x" << h << " NV12, stride=" << stride << ")\n";
    }

    const StreamConfiguration &sc_;
    Camera                    *camera_;
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
    /* 1640x1232 is the IMX219's 2x2-binned full-sensor mode.
     * It reads the entire 3280x2464 pixel array and bins down,
     * giving the full field of view at ~40fps.
     * 1280x720 is a partial-readout mode that crops the sensor
     * centre, which is why it looks zoomed. */
    sc.size        = {1640, 1232};
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

    FrameBufferAllocator alloc(camera);
    if (alloc.allocate(stream) < 0) {
        camera->release(); cm->stop(); return 1;
    }

    auto request = camera->createRequest();
    if (!request || request->addBuffer(stream, alloc.buffers(stream)[0].get())) {
        camera->release(); cm->stop(); return 1;
    }

    CaptureSession session(sc, camera.get());
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
