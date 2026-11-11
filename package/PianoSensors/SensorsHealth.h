/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU-piano contributors. All rights reserved. */
/* PianoPower's health report (PianoAdspContract.h, charging.md §9) as the guard's observation (SensorsGuard.h). The strike rules read
 * it exactly so: a fatal bit now or any fatal report since the start, a crash reason, the charger domain not up or in a new generation,
 * a new link generation, the DSP not up after we started; quiet holds our traffic. A report of another contract version than the one
 * built against counts as a fatal sign (the fields may mean something else). Header only, plain C; host-tested (tests/piano.c). */
#ifndef SENSORS_HEALTH_H
#define SENSORS_HEALTH_H
#include <PianoAdspContract.h>
#include "SensorsGuard.h"

static inline SgDsp sensors_dsp_of(const PianoAdspHealth *h)
{
    SgDsp d;
    const int known = h->version == PIANO_ADSP_HEALTH_VERSION;
    d.dsp_up = known && h->dsp_up ? 1 : 0;
    d.fatal = !known || h->fatal || h->fatal_reports ? 1 : 0;
    d.quiet = h->quiet ? 1 : 0;
    d.charger_state = h->charger_pd;
    d.charger_ups = h->charger_generation;
    d.link_generation = h->link_generation;
    d.crash = h->crash[0] ? h->crash : 0;
    return d;
}
#endif
