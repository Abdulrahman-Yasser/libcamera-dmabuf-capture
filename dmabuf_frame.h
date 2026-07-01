#pragma once
#include <cstdint>

struct DmaBufFrame {
    int fd = -1;                   // >= 0: DMA-BUF path (camera mode)
    const uint8_t *data = nullptr; // non-null: system memory path (file mode)
    int width = 0, height = 0;
    int stride = 0;
    int y_offset = 0, uv_offset = 0;
};
