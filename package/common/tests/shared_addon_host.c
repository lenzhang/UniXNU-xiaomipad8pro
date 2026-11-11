// SPDX-License-Identifier: BSD-2-Clause-Patent
// One boot addon for every user (docs/kc-space.md section 6): the container tooling/persist/firmware.py makes for a RAM image with
// PianoSensors' pack entry and firmware entries is accepted by both readers, each finding only its own entry.
//   shared_addon_host ADDON FIRMWARE_PATH
#include <stdio.h>
#include <string.h>
#include "SensorsAddon.h"
#include "PianoFirmwareCore.h"

int main(int argc, char **argv)
{
    static uint8_t buf[8 << 20];
    FILE *f = argc == 3 ? fopen(argv[1], "rb") : NULL;
    if (!f) return 2;
    const size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    SaEntry sensors;
    PfwAddonEntry fw;
    const int a = sa_find(buf, n, SA_PACK_PATH, &sensors), b = pfw_addon_find(buf, n, argv[2], &fw);
    const int c = sa_find(buf, n, argv[2], &sensors) == SA_OK, d = pfw_addon_find(buf, n, SA_PACK_PATH, &fw) == PIANO_FW_OK;
    if (a != SA_OK || b != PIANO_FW_OK || !c || !d) {
        printf("FAIL shared addon: sensors %d, firmware %d\n", a, b);
        return 1;
    }
    printf("PASS shared addon: PianoSensors' reader and the firmware loader both accept one container with both kinds of entries\n");
    return 0;
}
