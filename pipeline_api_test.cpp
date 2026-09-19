#include "pipeline_api.h"

#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char **argv)
{
    const char *config = argc > 1 ? argv[1] : "bev_config.ini";
    int seconds        = argc > 2 ? std::atoi(argv[2]) : 5;
    const char *snap   = argc > 3 ? argv[3] : "/tmp/pipeline_test.png";

    PipelineHandle *h = pipeline_create(config, nullptr);
    if (pipeline_start(h) != 0) {
        std::fprintf(stderr, "pipeline_start failed\n");
        pipeline_destroy(h);
        return 1;
    }

    PipelineFrame f{};
    uint64_t first = 0, last = 0;
    auto t0 = std::chrono::steady_clock::now();
    bool snapped = false;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(seconds)) {
        if (pipeline_get_frame(h, &f) == 0) {
            if (!first) {
                first = f.frame_id;
                std::printf("frame %dx%d stride=%d fourcc=0x%x modifier=0x%llx\n",
                            f.width, f.height, f.stride, f.fourcc,
                            (unsigned long long)f.modifier);
            }
            last = f.frame_id;
            close(f.dmabuf_fd);
            if (!snapped) {
                pipeline_save_snapshot(h, snap);
                snapped = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("frames rendered: %llu in %.1fs (%.1f fps)\n",
                (unsigned long long)(last - first), secs, (last - first) / secs);
    std::printf("snapshot: %s\n", snap);

    pipeline_stop(h);
    pipeline_destroy(h);
    return last > first ? 0 : 2;
}
