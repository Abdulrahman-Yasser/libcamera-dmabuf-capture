# libcamera-dmabuf-capture

A small test program I wrote as part of the AGL AI Backup Camera GSoC project.
Before building the full GPU pipeline I needed to verify that libcamera works
correctly on the RPi4 AGL image and actually gives back a DMA-BUF file
descriptor I can later pass to the GPU.

## What it does

- Opens the first camera found by libcamera
- Configures NV12 output at 1280x720
- Waits 30 frames for auto-exposure and auto-white-balance to settle
- Prints the DMA-BUF fd and plane layout for the captured frame
- Saves the raw NV12 data to `/tmp/frame.raw`

## Build

The AGL image doesn't have a compiler on it, so I cross-compile from the host
using the toolchain that Yocto already built:

```bash
chmod +x cross-build.sh
./cross-build.sh
```

Needs an existing AGL RPi4 Yocto build at
`/media/abdu/LinuxHome/Embedded_Linux/git_ignoring/AGL/raspberrypi4/`.

## Deploy and run

```bash
scp libcamera-dmabuf-capture root@<rpi-ip>:/home/
ssh root@<rpi-ip> ./libcamera-dmabuf-capture
```

Expected output:

```
[capture] camera: /base/soc/i2c0mux/i2c@1/imx219@10
[capture] sensor active area: (0, 0)/3280x2464
[capture] warming up AE/AWB (30 frames)...
=== Frame captured (frame 31) ===
  Width      : 1280
  Height     : 720
  Stride     : 1280
  PixelFormat: NV12
  Planes     : 2
  Plane[0]:  fd=15  offset=0  length=921600
  Plane[1]:  fd=15  offset=921600  length=460800
[capture] raw frame saved to /tmp/frame.raw
```

## View the raw frame

```bash
scp root@<rpi-ip>:/tmp/frame.raw .
ffplay -f rawvideo -pixel_format nv12 -video_size 1280x720 frame.raw
```

## Note on ScalerCrop

At 1280x720, libcamera picks a partial-readout sensor mode on the IMX219 that
only uses the centre of the sensor — the result looks zoomed in. Querying
`PixelArrayActiveAreas` and setting `controls::ScalerCrop` to the full
3280x2464 area on each request tells the ISP to read the whole sensor and
scale it down, giving the correct wide-angle field of view.
