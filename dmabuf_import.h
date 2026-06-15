#pragma once

#include "egl_context.h"
#include <libcamera/libcamera.h>

bool import_and_save_png(const EGLState                          &egl,
                         const libcamera::FrameBuffer            *buf,
                         const libcamera::StreamConfiguration    &sc);
