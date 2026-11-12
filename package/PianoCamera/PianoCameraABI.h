// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoCamera's user client (level 5's kernel side, first version: one capture per call; docs/drivers/camera.md section 4.5). C,
// shared by the kext and its user-space clients (tooling/native/piano-camera-grab, later the camera extension).
//
//   IOServiceOpen(the "PianoCamera" service, task, 0, &connect)          an administrator only (this version)
//   IOConnectCallStructMethod(connect, kPianoCameraInfo, NULL, 0, &info, &size)          the service and the last capture
//   IOConnectCallStructMethod(connect, kPianoCameraCapture, &args, sizeof args, &info, &size)
//        powers the sensor up, streams with the exposure loop, keeps the last frame, powers everything down again (a few
//        seconds; one capture at a time: kIOReturnBusy otherwise); info describes the frame
//   IOConnectMapMemory64(connect, kPianoCameraMemoryRaw, ...)                the last capture's RAW10 frame, MIPI-packed: info.height lines
//        of info.stride bytes (4 pixels in 5 bytes: their 8 high bits, then a byte of their 2 low bits, pixel 0 in bits 1:0)
#pragma once
#include <stdint.h>

#define kPianoCameraABIVersion 1u

enum {
  kPianoCameraInfo = 0,
  kPianoCameraCapture = 1,
};
enum { kPianoCameraMemoryRaw = 0 };
enum { kPianoCameraRear = 1, kPianoCameraFront = 2 };
// The Bayer order of the frame's first two lines (CameraHw.h: the rear S5KJN1 unflipped GRBG, the c3r colour bars confirm it; the
// front OV32D40 BGGR, its driver's media bus code).
enum { kPianoCameraRGGB = 0, kPianoCameraGRBG = 1, kPianoCameraGBRG = 2, kPianoCameraBGGR = 3 };

typedef struct {
  uint32_t version;     // kPianoCameraABIVersion
  uint32_t sensor;      // kPianoCameraRear or kPianoCameraFront
  uint32_t pattern;     // the rear's colour bars (1) or the scene (0)
  uint32_t ae;          // the exposure loop (1) or the drivers' defaults (0)
  uint32_t aeTarget;    // its target above the black level (0: the kext's piano-camera-ae-target)
  uint32_t timeoutMs;   // how long the call waits (0: 20000)
} PianoCameraCaptureArgs;

typedef struct {
  uint32_t version;     // kPianoCameraABIVersion
  uint32_t status;      // 0: a frame is in memory 0; else the step the capture failed at (3xx, 2xx), or 1 (no capture yet)
  uint32_t sensor, width, height, stride, bayer, black;
  uint64_t bytes;       // stride x height
  uint32_t exposure, gain, gainUnit;   // the frame's controls (gainUnit: the gain code of 1x)
  uint32_t aeVerdict, aeSamples;       // CameraStream.h AeVerdict (0 not run, 1 converged, 2 dark, 3 dim, 4 saturated, ...)
  uint32_t means[4];    // the frame's mean per Bayer position (with the black level)
  uint32_t sequence;    // captures since boot
  uint32_t level;       // piano-camera's level
  uint64_t timeNs;      // when the frame was kept (mach_absolute_time in ns)
  char verdict[96];     // the result line's text
} PianoCameraFrameInfo;
