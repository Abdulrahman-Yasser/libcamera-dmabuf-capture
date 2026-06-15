# libcamera-dmabuf-capture

A small test program I wrote as part of the AGL AI Backup Camera GSoC project.
Before building the full GPU pipeline I needed to verify that libcamera works
correctly on the RPi4 AGL image and actually gives back a DMA-BUF file
descriptor I can later pass to the GPU.

## What it does

- Opens the first camera found by libcamera
- Configures NV12 output at 1640x1232 (IMX219 full-sensor 2x2-binned mode)
- Waits 30 frames for auto-exposure and auto-white-balance to settle
- Prints the DMA-BUF fd and plane layout for the captured frame
- Saves the raw NV12 data to `/tmp/frame.raw` (intermediate debug output)
- Imports the DMA-BUF fd into EGL as two `GL_TEXTURE_EXTERNAL_OES` textures
  (Y plane as `DRM_FORMAT_R8`, UV plane as `DRM_FORMAT_RG88`)
- Runs a GLSL ES 2.0 NV12→RGB shader into an FBO, reads back with `glReadPixels`
- Saves the colour result as `/tmp/frame.png` via libpng

## Build

The AGL image doesn't have a compiler on it, so I cross-compile from the host
using the toolchain that Yocto already built:

```bash
./cross-build.sh
```

The script runs:

```bash
source /opt/agl-sdk/21.90.0-aarch64-agl-image-flutter-debug/environment-setup-aarch64-agl-linux
mkdir -p build
cd build
cmake -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN_FILE" ..
make -j$(nproc)
```

The SDK `environment-setup` script sets `$CMAKE_TOOLCHAIN_FILE` and all the
cross-compiler environment variables. The binary ends up at `build/libcamera-dmabuf-capture`.

## Deploy and run

```bash
scp libcamera-dmabuf-capture root@<rpi-ip>:/home/
ssh root@<rpi-ip> ./libcamera-dmabuf-capture
```

Expected output:

```
[capture] camera: /base/soc/i2c0mux/i2c@1/imx219@10
[capture] warming up AE/AWB (30 frames)...
=== Frame captured (frame 31) ===
  Width      : 1640
  Height     : 1232
  Stride     : 1664
  PixelFormat: NV12
  Planes     : 2
  Plane[0]:  fd=15  offset=0  length=2049920
  Plane[1]:  fd=15  offset=2049920  length=1024960
[capture] raw frame saved to /tmp/frame.raw (1640x1232 NV12, stride=1664)
```

## View the raw frame (NV12 debug output)

```bash
scp root@<rpi-ip>:/tmp/frame.raw .
ffplay -f rawvideo -pixel_format nv12 -video_size 1640x1232 frame.raw
```

## Verify the PNG (Stage 3 output)

```bash
scp root@<rpi-ip>:/tmp/frame.png .
xdg-open frame.png   # or: feh frame.png / open frame.png (macOS)
```

The PNG should show a recognisable colour image from the camera.
If it looks correct, the full DMA-BUF → EGL → GPU pipeline is working.

## Why 1640x1232 and not 1280x720

The IMX219 has several native sensor readout modes. At 1280×720, libcamera
picks a partial-readout mode that physically reads out only the centre of the
pixel array — the image looks zoomed in even though the lens is not zoomed.

1640×1232 is the sensor's 2×2-binned mode: it reads the full 3280×2464 pixel
array and bins every 2×2 block into one output pixel, giving the correct
wide-angle field of view at full speed (~40fps).

