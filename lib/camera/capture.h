#pragma once

/*
 * The two choices a capture backend is opened with: where frames come
 * from and which pixel format to ask for. Shared by v4l2_in.h and
 * rpicam_in.h and by whatever configures them (eeye's config.h), so the
 * backends need nothing of any one program's configuration.
 */

// Where frames come from. See rpicam_in.h for why a Pi CSI camera needs a
// separate backend rather than another device path handed to v4l2_in.c.
typedef enum {
  CAPTURE_AUTO = 0, // probe for a CSI camera, else V4L2
  CAPTURE_V4L2,     // a USB/UVC webcam, via v4l2_in.c
  CAPTURE_RPICAM,   // a Pi CSI camera module, via rpicam_in.c
  // A frame bus stream (eeye's frame bus, legacy/eeye/fbus.h) -- frames
  // another program publishes, e.g. hellion decoding the ROV's camera
  // topside. Never chosen by "auto".
  CAPTURE_BUS,
} CaptureSource;

// Which pixel format to ask a V4L2 camera for. Only meaningful on the
// V4L2 backend; the Pi camera path always delivers MJPEG (see rpicam_in.h).
//
// This exists because the two formats have very different costs. YUYV is
// packed 4:2:2 -- the same layout VideoFrame uses, just interleaved -- so
// capture is a deinterleave into the planes with NO decode at all. MJPEG
// must be decoded every frame. On a camera that offers both, which one
// you get should be a decision, not a side effect of probe order.
//
// The tradeoff is bandwidth: YUYV is uncompressed, so 1280x720 at 30fps
// needs ~55 MB/s and will not fit USB 2.0's isochronous ceiling. That is
// why FORMAT_AUTO still tries MJPEG first -- it is the right default for
// the cameras most people have.
typedef enum {
  FORMAT_AUTO = 0, // MJPEG if offered at this size, else YUYV
  FORMAT_MJPEG,    // MJPEG only; fail at startup if unavailable
  FORMAT_YUYV,     // YUYV only; fail at startup if unavailable
  // rpicam backend only: take the ISP's raw 4:2:0 output instead of having
  // rpicam-vid software-encode MJPEG that eeye then decodes. Opt-in until
  // verified on real hardware -- see rpicam_in.c.
  FORMAT_YUV420,
} CaptureFormat;
