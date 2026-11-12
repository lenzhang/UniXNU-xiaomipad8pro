# Camera sensor register tables (data only)

The register tables piano's two camera sensors need to stream: the rear Samsung **S5KJN1** (`s5kjn1.tbl`) and the front
OmniVision **OV32D40** (`ov32d40.tbl`). PianoCamera (`package/PianoCamera`, `docs/drivers/camera.md`) reads them from level 3
on, through the C header `tooling/camera/sensor-tables.py header` generates from this directory at build time.

## Provenance

| Table | Taken from | Pinned file (SHA-256) |
|---|---|---|
| `s5kjn1.tbl` | the Ubuntu piano kernel `yzddmr6/linux-sm8750-piano`, branch `piano/dev`, commit `7f8d6bedde479a01f4a05d6d0581b43c7fd20a01` (the v1.2.0 merge; Linux 7.2): `drivers/media/i2c/s5kjn1.c` (Samsung S5KJN1 driver, Linaro, GPL-2.0) | `4d1e2dad37d06fb594ac044903fe2eb674b3bf1ab3135da56913b53d6b244a92` |
| `ov32d40.tbl` | the same commit: `drivers/media/i2c/ov32d40.c` (OmniVision OV32D40 driver, GPL-2.0-only, added to the piano kernel by K:ad086b0546b0) | `f784a23f088de3dccc165d61b7ebbce6484c4f7a7c0420cf901ede417ffba684` |

These are the drivers Ubuntu v0.3.0 brought both cameras up with (K:ad086b0546b0 added the OV32D40; v0.10.0 K:74840fbc9af1,
K:8e914bcc19ca exposed the S5KJN1 gain code and restated the OV32D40 timing units; the tables are those of the pinned commit; `docs/ubuntu-history/kernel.md` §1.10). The same two
files are in the local reference tree the other drivers cite (byte-identical).

**What is carried, and on what terms.** Owner decision of 2026-11-12 (`docs/BACKLOG-hardware.md` §4): the sensors' register and
calibration tables are hardware data, and are kept in this repository as data. Only the data is here: each array's register
addresses, widths and values in the drivers' order, the modes' timing parameters (width, height, line/frame lengths, default
exposure and its margin), and the few register writes the drivers make outside their arrays (a stream-start preamble, the stream
on/off bit), each named by the function it was read from in `tooling/camera/sensor-tables.py`. No function, comment, structure
or other code of the drivers is copied; the files are in this repository's own format (below). Nothing in these tables is a
per-unit calibration: they are the same for every piano. A unit's own data (lens shading, OTP dumps of this tablet's sensors) is
never committed; it would stay local, as the touch calibration does.

## Format

Text, one item a line, `#` comments:

```
sensor NAME                 the sensor
source PATH SHA256          the driver file the data was read from, at the pinned commit
[KIND NAME]                 a section: start | init | mode WxH | on | off (written in that order to start a stream; off stops it)
p KEY VALUE                 a mode parameter, decimal
w BITS ADDR VALUE           a register write: 8, 16 or 24 bits, hexadecimal address and value
d MIN_US MAX_US             a delay before the next write
```

Between the mode and `on`, PianoCamera writes its own controls (exposure, analogue gain, frame length, the rear's flips): those
registers are listed with the sensor facts in `package/PianoCamera/CameraHw.h`, not here.

## Tools

```sh
tooling/camera/fetch-ubuntu-camera-sources.sh                   # the pinned sources into out/upstream/ (ignored), hashes checked
python3 tooling/camera/sensor-tables.py check data/camera --src out/upstream/k   # MANIFEST + format + a fresh extraction equals these files
python3 tooling/camera/sensor-tables.py check data/camera       # MANIFEST + format only (the build runs this)
python3 tooling/camera/sensor-tables.py extract out/upstream/k data/camera        # rewrite the tables (then review, then MANIFEST)
```

`MANIFEST` pins each file's SHA-256 and every section's write count; the build refuses tables that differ from it.
