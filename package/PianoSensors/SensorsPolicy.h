/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* Piano's sensor facts and the macOS-side policies over the hub's samples (docs/drivers/sensors.md sections 2 and 5): which data types
 * and DSP resources, the accelerometer's axes in the panel's frame, the ambient light curve PianoBacklight is fed, and the display
 * rotation with the keyboard cover's lock. Portable C, integer only (kernel code builds with -mgeneral-regs-only), no C library;
 * host-tested (tests/piano.c). The kext and the rotation agent use the same functions. */
#ifndef SENSORS_POLICY_H
#define SENSORS_POLICY_H
#include <stdint.h>
#include "SensorsFixed.h"
#include "SensorsOrient.h"
#ifdef __cplusplus
extern "C" {
#endif

/* ---- facts (sources in sensors.md section 2) ---- */
#define PS_GLINK_CHANNEL "fastrpcglink-apps-dsp"   /* sm8750.dtsi fastrpc node; the DSP opens it natively (run 2026-11-11) */
#define PS_QMI_SNS 0x190u                           /* the sensor client service (SSC/QSH), QMI over QRTR */
#define PS_PD_SENSORS 2u                            /* Linux fastrpc.c SENSORS_PD; FASTRPC_IOCTL_INIT_ATTACH_SNS */
#define PS_CONTEXT_BANK 3u                          /* compute-cb@3 (reg 3), the first session Linux hands out on piano's DT */
#define PS_STREAM_A 0x1003u                         /* compute-cb@3 iommus: 0x1003 mask 0x80, 0x1043 mask 0x20 */
#define PS_STREAM_A_MASK 0x80u
#define PS_STREAM_B 0x1043u
#define PS_STREAM_B_MASK 0x20u
#define PS_TYPE_ACCEL "accel"                       /* ST LSM6DSO (odm config lsm6dso_0) */
#define PS_TYPE_GYRO "gyro"                         /* the same LSM6DSO */
#define PS_TYPE_MAG "mag"                           /* QST QMC6308 (qmc6308_0): unused on macOS */
#define PS_TYPE_LIGHT "ambient_light"               /* Sensortek STK3BCX front (stk3bcx_0); SIP1328 rear is ambient_light_back */
#define PS_TYPE_PROXIMITY "proximity"               /* the STK3BCX's: unused on macOS */
#define PS_TYPE_REGISTRY "registry"                 /* present once the sensor PD has its registry (libssc's readiness probe) */
/* The accelerometer in the panel's frame (landscape, camera edge up = "top"), in SensorsFixed's axis-map text, applied after
 * sfx_accel_ios's sign reversal: Ubuntu's mount matrix (x, y, z) = (y, -x, z) with iio-sensor-proxy 3.9's orientation rule, i.e. the
 * cover-docked position (SSC +x up) is the normal one. Inferred: Ubuntu never ran the four-edge test either (piano-sensors-axes). */
#define PS_ACCEL_AXES "a+y+x+z"

/* ---- ambient light -> backlight (the curve, smoothing and hysteresis; PianoBacklight adds the user's offset and its ramps) ---- */
typedef struct {
    uint32_t lux, permille;              /* permille of PianoBacklight's percent scale */
} PsCurvePoint;
extern const PsCurvePoint ps_als_curve[];
extern const unsigned ps_als_curve_points;
/* The curve at lux (piecewise linear between the points, flat beyond the ends). 0 lux is valid darkness (Ubuntu's ambient-zero). */
uint32_t ps_als_curve_permille(uint32_t lux);

#define PS_ALS_HYSTERESIS 30u            /* permille: the goal moves only when the curve differs this much (noise at a corner) */
#define PS_ALS_STEP 10u                  /* permille: smaller changes of the smoothed value are not published */
#define PS_ALS_UP_MS 1000u               /* smoothing time constant towards a brighter goal */
#define PS_ALS_DOWN_MS 4000u             /* ... towards a darker one */
#define PS_ALS_PUBLISH_MS 250u           /* at most one target this often */
typedef struct {
    int has;                             /* a sample came */
    uint32_t goal;                       /* permille the smoothed value moves towards */
    uint32_t smooth_q16;                 /* permille << 16 */
    uint32_t published;                  /* the last target given out (UINT32_MAX: none) */
    uint64_t last_ms, published_ms;
} PsAls;
void ps_als_init(PsAls *a);
/* A sample (lux in thousandths, as sfx_milli gives it; negative is darkness), or a tick without one (have_sample 0: the hub's light
 * stream is on-change, the smoothing goes on between events; call it every PS_ALS_PUBLISH_MS). The target permille to send now, or -1. */
int32_t ps_als_step(PsAls *a, int have_sample, int32_t milli_lux, uint64_t now_ms);

/* ---- accelerometer -> panel edge -> display rotation ---- */
enum {PS_EDGE_NONE = -1, PS_EDGE_TOP = 0, PS_EDGE_LEFT = 1, PS_EDGE_BOTTOM = 2, PS_EDGE_RIGHT = 3};
/* The panel edge that is up for an orientation state (SensorsOrient.h, fed in the panel frame of PS_ACCEL_AXES); flat, face up/down and
 * unknown are PS_EDGE_NONE (keep what is shown). */
int ps_edge_of(int sno_state);
const char *ps_edge_name(int edge);
/* One accelerometer sample: the hub's three floats (m/s^2, bits), mapped and given to the orientation state machine. The new state
 * when it changed, else -1. */
int ps_accel_sample(SnoState *s, const SfxAxes *axes, const uint32_t bits[3], uint64_t now_ms);

typedef struct {
    uint16_t degrees[4];                 /* the rotation for each edge up (TOP, LEFT, BOTTOM, RIGHT): 0, 90, 180, 270 by default */
    uint32_t settle_ms;                  /* an edge must stay up this long (after SensorsOrient's own 250 ms) */
    uint32_t min_interval_ms;            /* between two rotations */
} PsRotationConfig;
void ps_rotation_defaults(PsRotationConfig *c);
typedef struct {
    int rotation;                        /* degrees last asked for, -1 before the first */
    int edge;                            /* the edge seen last, and since when */
    uint64_t edge_since, changed_ms;
} PsRotation;
void ps_rotation_init(PsRotation *r);
/* The rotation to apply now (0/90/180/270), or -1 to keep the current one. attached: the keyboard cover is attached (then always 0,
 * Ubuntu's rule: docked is the normal landscape); locked: the user's rotation lock (keep whatever is shown). */
int ps_rotation_step(PsRotation *r, const PsRotationConfig *c, int edge, int attached, int locked, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
