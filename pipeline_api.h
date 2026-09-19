#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PipelineHandle PipelineHandle;

typedef struct {
    int      dmabuf_fd;
    int      width;
    int      height;
    int      stride;
    int      fourcc;
    uint64_t modifier;
    uint64_t frame_id;
} PipelineFrame;

PipelineHandle *pipeline_create(const char *bev_config_path, const char *tuning_file);
int  pipeline_start(PipelineHandle *h);
int  pipeline_get_frame(PipelineHandle *h, PipelineFrame *out);
int  pipeline_set_calibration(PipelineHandle *h, int slot,
                              double cam_x_delta, double cam_y_delta, double yaw_delta_deg);

int  pipeline_save_snapshot(PipelineHandle *h, const char *path);

void pipeline_stop(PipelineHandle *h);
void pipeline_destroy(PipelineHandle *h);

#ifdef __cplusplus
}
#endif
