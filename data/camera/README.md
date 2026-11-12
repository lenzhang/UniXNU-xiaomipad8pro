# Camera register tables (data only)

The register tables piano's two camera sensors need to stream: the rear Samsung **S5KJN1** (`s5kjn1.tbl`) and the front
OmniVision **OV32D40** (`ov32d40.tbl`); and the lane table of SM8750's **CSIPHY** (2-phase D-PHY, v2.3/2.4) that receives them
(`csiphy-v2.4.tbl`). PianoCamera (`package/PianoCamera`, `docs/drivers/camera.md`) reads them from level 3 on, through the C
header `tooling/camera/sensor-tables.py header` generates from this directory at build time.

## Provenance

| Table | Taken from | Pinned file (SHA-256) |
|---|---|---|
| `s5kjn1.tbl` | the Ubuntu piano kernel `yzddmr6/linux-sm8750-piano`, branch `piano/dev`, commit `7f8d6bedde479a01f4a05d6d0581b43c7fd20a01` (the v1.2.0 merge; Linux 7.2): `drivers/media/i2c/s5kjn1.c` (Samsung S5KJN1 driver, Linaro, GPL-2.0) | `4d1e2dad37d06fb594ac044903fe2eb674b3bf1ab3135da56913b53d6b244a92` |
| `ov32d40.tbl` | the same commit: `drivers/media/i2c/ov32d40.c` (OmniVision OV32D40 driver, GPL-2.0-only, added to the piano kernel by K:ad086b0546b0) | `f784a23f088de3dccc165d61b7ebbce6484c4f7a7c0420cf901ede417ffba684` |
| `csiphy-v2.4.tbl` | the same commit: `drivers/media/platform/qcom/camss/camss-csiphy-3ph-1-0.c` (Qualcomm CAMSS CSIPHY driver, The Linux Foundation / Linaro, GPL-2.0): the lane array its init function selects for `CAMSS_8750` (the "3nm 2PH v2.3.0/2.4.0 2p5Gbps 4 lane DPHY mode" array, which the driver says v2.4.0 shares with v2.3.0), and the two block offsets that case sets (common block `0x1000`, its status words at `+0x138`) | `51c1101c69c5858d0a72cec74e810ea21c85561b170818ef786c198ce31ce12f` |

**The CSIPHY table** is a PHY tuning table (vendor-tuned analogue settings), not a sensor's: the owner's decision of 2026-11-12
covers it as well (PHY tuning tables from the Linux drivers are committed as data in their own directory, with this note; the same
decision covers the DisplayPort PHY's tables). Piano's buses are D-PHY: the DT's CSIPHY endpoints (`sm8750-xiaomi-piano-camera.dtsi`,
ports 1 and 4) give `data-lanes = <0 1 2 3>` and no bus type, and the sensors' endpoints four data lanes. The table is 103 entries
in the driver's order: four data lanes (offsets `0x000`, `0x400`, `0x800`, `0xc00`) of 21 entries and the clock lane (`0xe00`) of
19; five entries carry a delay (209 us); the five `settle` entries (one per lane) take the settle count computed from the link rate; the 16 `skew`
entries (skew calibration) are not written, as the driver does not write them. The common block's own writes around the table (its
reset, the lane-enable mask, the power-up and the interrupt masks) are code facts, with their source, in
`package/PianoCamera/CameraHw.h`, not data here.

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

The PHY table:

```
phy NAME                    the PHY (csiphy-v2.4)
source PATH SHA256          the driver file the data was read from, at the pinned commit
[lanes NAME]                the lane table of one mode (dphy-4lane)
p soc CAMSS_8750            the driver's SoC case that selects this array
p common 4096               the common block's offset in the PHY (decimal)
p status 312                the common status words' offset from the common block (decimal)
r OFFSET VALUE DELAY KIND   one entry in the driver's order: offset in the PHY block, 8-bit value (hexadecimal), delay after it
                            (decimal us), kind: default | settle (the settle count replaces the value) | skew (not written) |
                            dnp (not written) | lane-enable | settle-hi | 2ph | 3ph
```

## Tools

```sh
tooling/camera/fetch-ubuntu-camera-sources.sh                   # the pinned sources into out/upstream/ (ignored), hashes checked
python3 tooling/camera/sensor-tables.py check data/camera --src out/upstream/k   # MANIFEST + format + a fresh extraction equals these files
python3 tooling/camera/sensor-tables.py check data/camera       # MANIFEST + format only (the build runs this)
python3 tooling/camera/sensor-tables.py extract out/upstream/k data/camera        # rewrite the tables (then review, then MANIFEST)
```

`MANIFEST` pins each file's SHA-256 and every section's entry count (a sensor section's writes, the PHY section's lane entries);
the build refuses tables that differ from it.
