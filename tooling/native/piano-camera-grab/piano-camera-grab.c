// SPDX-License-Identifier: BSD-2-Clause-Patent
// piano-camera-grab: one capture per sensor from PianoCamera's user client (level 5, kernel boot argument piano-camera=5;
// docs/drivers/camera.md section 4.5), its RAW10 frame through the first software ISP (package/PianoCamera/CameraIsp.h: quads, grey
// world, a digital gain, sRGB) to a PPM, its numbers and a thumbnail as lines on stdout. An administrator only (the kext checks).
//   piano-camera-grab [front|rear|both] [--pattern] [--no-ae] [--target N] [--scale N] [--out DIR] [--thumb W] [--raw]
//   piano-camera-grab --boot      the launchd job's mode: exits at once unless the kernel's boot arguments have piano-camera-grab=1;
//                                 waits for level 5 (at most 300 s), captures the front, then the rear, as `both`
// Files: DIR/piano-camera-<sensor>.ppm (default DIR /private/var/tmp; --raw also .raw10, the frame as the kext kept it). Lines:
//   piano-camera-grab: <sensor>: <the kext's result line>; exposure E gain G (x.xx); WxH stride S; means a/b/c/d
//   piano-camera-grab: <sensor> isp: WxH (scale N) means r/g/b, white balance r/b, digital gain k -> FILE
//   piano-camera-grab: t <sensor> <W>x<H> r<row> p<part> <hex RGB of up to 24 pixels>     (tooling/camera/console-thumb.py)
// Exit status 0 when every capture asked for gave a frame.
// Build on the Mac (arm64, ad-hoc signed by the linker; configs/userland.toml):
//   cc -O2 -Wall -Wextra -I package/PianoCamera -o piano-camera-grab piano-camera-grab.c -framework IOKit -framework CoreFoundation
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "CameraIsp.h"
#include "PianoCameraABI.h"

static const char *sensorName(uint32_t s) { return s == kPianoCameraFront ? "front" : "rear"; }

static int bootArgHas(const char *arg)
{
  char args[2048];
  size_t n = sizeof args;
  if (sysctlbyname("kern.bootargs", args, &n, NULL, 0) || !n) return 0;
  args[n < sizeof args ? n : sizeof args - 1] = 0;
  const size_t len = strlen(arg);
  for (const char *p = args; (p = strstr(p, arg)); p += len)
    if ((p == args || p[-1] == ' ') && (p[len] == ' ' || p[len] == 0)) return 1;
  return 0;
}

// The service, its level 5 ready: open, then a capture attempt answers kIOReturnNotReady until the camera thread serves.
static io_connect_t openCamera(unsigned waitSeconds)
{
  for (unsigned t = 0;; ++t) {
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("PianoCamera"));
    if (s) {
      io_connect_t c = 0;
      const kern_return_t r = IOServiceOpen(s, mach_task_self(), 0, &c);
      IOObjectRelease(s);
      if (r == KERN_SUCCESS) return c;
      if (t == 0) printf("piano-camera-grab: IOServiceOpen: %#x (level 5 is piano-camera=5; root only)\n", r);
    }
    if (t >= waitSeconds) return 0;
    sleep(1);
  }
}

static int writePpm(const char *path, const uint8_t *rgb, uint32_t w, uint32_t h)
{
  FILE *f = fopen(path, "wb");
  if (!f) return -1;
  fprintf(f, "P6\n%u %u\n255\n", w, h);
  const size_t n = (size_t)w * h * 3;
  const int ok = fwrite(rgb, 1, n, f) == n;
  return fclose(f) == 0 && ok ? 0 : -1;
}

static void printThumb(const char *name, const uint8_t *rgb, uint32_t w, uint32_t h, uint32_t tw)
{
  if (!tw) return;
  const uint32_t th = (uint32_t)((uint64_t)h * tw / w);
  uint8_t *t = malloc((size_t)tw * th * 3);
  if (!t) return;
  pianoThumb(rgb, w, h, tw, th, t);
  static const char hex[] = "0123456789abcdef";
  for (uint32_t y = 0; y < th; ++y)
    for (uint32_t x0 = 0, part = 0; x0 < tw; x0 += 24, ++part) {
      char line[24 * 6 + 1];
      uint32_t n = 0;
      for (uint32_t x = x0; x < x0 + 24 && x < tw; ++x)
        for (int c = 0; c < 3; ++c) {
          const uint8_t v = t[((size_t)y * tw + x) * 3 + c];
          line[n++] = hex[v >> 4];
          line[n++] = hex[v & 15];
        }
      line[n] = 0;
      printf("piano-camera-grab: t %s %ux%u r%u p%u %s\n", name, tw, th, y, part, line);
    }
  fflush(stdout);
  free(t);
}

static int grab(io_connect_t c, uint32_t sensor, int pattern, int ae, uint32_t target, uint32_t scale, const char *dir, uint32_t thumb,
                int raw)
{
  const char *name = sensorName(sensor);
  PianoCameraCaptureArgs a = {kPianoCameraABIVersion, sensor, (uint32_t)pattern, (uint32_t)ae, target, 30000};
  PianoCameraFrameInfo info;
  size_t size = sizeof info;
  kern_return_t r;
  for (unsigned t = 0;; ++t) {
    size = sizeof info;
    r = IOConnectCallStructMethod(c, kPianoCameraCapture, &a, sizeof a, &info, &size);
    if (r != kIOReturnNotReady || t >= 300) break;
    sleep(1);   // the camera thread is still in its delay or survey
  }
  if (r != KERN_SUCCESS) {
    printf("piano-camera-grab: %s: capture: %#x\n", name, r);
    return -1;
  }
  printf("piano-camera-grab: %s: %s; exposure %u gain %u (%u.%02ux); %ux%u stride %u; means %u/%u/%u/%u; status %u\n", name, info.verdict,
         info.exposure, info.gain, info.gainUnit ? info.gain / info.gainUnit : 0, info.gainUnit ? info.gain % info.gainUnit * 100 / info.gainUnit : 0,
         info.width, info.height, info.stride, info.means[0], info.means[1], info.means[2], info.means[3], info.status);
  if (info.status || !info.bytes) return -1;
  mach_vm_address_t va = 0;
  mach_vm_size_t len = 0;
  r = IOConnectMapMemory64(c, kPianoCameraMemoryRaw, mach_task_self(), &va, &len, kIOMapAnywhere | kIOMapReadOnly);
  if (r != KERN_SUCCESS || len < info.bytes) {
    printf("piano-camera-grab: %s: map: %#x (%llu bytes)\n", name, r, (unsigned long long)len);
    return -1;
  }
  const uint8_t *frame = (const uint8_t *)(uintptr_t)va;
  char path[512];
  if (raw) {
    snprintf(path, sizeof path, "%s/piano-camera-%s.raw10", dir, name);
    FILE *f = fopen(path, "wb");
    if (f) {
      fwrite(frame, 1, info.bytes, f);
      fclose(f);
    }
  }
  const uint32_t ow = info.width / 2 / scale, oh = info.height / 2 / scale;
  float *work = malloc((size_t)ow * oh * 3 * sizeof(float));
  uint8_t *rgb = malloc((size_t)ow * oh * 3);
  PianoIspStats st;
  int rc = -1;
  if (work && rgb && pianoIsp(frame, info.width, info.height, info.stride, info.bayer, info.black, scale, work, rgb, &st) == 0) {
    snprintf(path, sizeof path, "%s/piano-camera-%s.ppm", dir, name);
    rc = writePpm(path, rgb, st.width, st.height);
    printf("piano-camera-grab: %s isp: %ux%u (scale %u) means %.1f/%.1f/%.1f above black, white balance %.2f/%.2f, digital gain %.2f -> %s%s\n",
           name, st.width, st.height, scale, st.mean[0], st.mean[1], st.mean[2], st.gain[0], st.gain[2], st.digital, path,
           rc ? " (NOT written)" : "");
    fflush(stdout);
    printThumb(name, rgb, st.width, st.height, thumb);
  }
  free(work);
  free(rgb);
  IOConnectUnmapMemory64(c, kPianoCameraMemoryRaw, mach_task_self(), va);
  return rc;
}

int main(int argc, char **argv)
{
  int pattern = 0, ae = 1, raw = 0, boot = 0;
  uint32_t target = 0, scale = 2, thumb = 64, which = 3;   // 1 rear, 2 front, 3 both (the front first)
  const char *dir = "/private/var/tmp";
  for (int i = 1; i < argc; ++i) {
    const char *a = argv[i];
    if (!strcmp(a, "front")) which = 2;
    else if (!strcmp(a, "rear")) which = 1;
    else if (!strcmp(a, "both")) which = 3;
    else if (!strcmp(a, "--pattern")) pattern = 1;
    else if (!strcmp(a, "--no-ae")) ae = 0;
    else if (!strcmp(a, "--raw")) raw = 1;
    else if (!strcmp(a, "--boot")) boot = 1;
    else if (!strcmp(a, "--target") && i + 1 < argc) target = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (!strcmp(a, "--scale") && i + 1 < argc) scale = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (!strcmp(a, "--thumb") && i + 1 < argc) thumb = (uint32_t)strtoul(argv[++i], NULL, 10);
    else if (!strcmp(a, "--out") && i + 1 < argc) dir = argv[++i];
    else {
      fprintf(stderr, "usage: piano-camera-grab [front|rear|both] [--pattern] [--no-ae] [--target N] [--scale N] [--out DIR] "
                      "[--thumb W] [--raw] | --boot\n");
      return 2;
    }
  }
  if (!scale || scale > 16) scale = 2;
  if (thumb > 256) thumb = 256;
  setvbuf(stdout, NULL, _IOLBF, 0);
  if (boot && !bootArgHas("piano-camera-grab=1")) return 0;
  if (boot) printf("piano-camera-grab: --boot: waiting for PianoCamera's level 5\n");
  mkdir(dir, 0755);
  io_connect_t c = openCamera(boot ? 300 : 0);
  if (!c) {
    printf("piano-camera-grab: no PianoCamera user client (piano-camera=5, as root)\n");
    return 1;
  }
  int failed = 0;
  if (which & 2) failed |= grab(c, kPianoCameraFront, 0, ae, target, scale, dir, thumb, raw) != 0;
  if (which & 1) failed |= grab(c, kPianoCameraRear, pattern, ae, target, scale, dir, thumb, raw) != 0;
  IOServiceClose(c);
  printf("piano-camera-grab: done%s\n", failed ? " (a capture failed)" : "");
  return failed;
}
