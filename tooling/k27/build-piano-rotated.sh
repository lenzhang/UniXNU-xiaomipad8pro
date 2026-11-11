#!/bin/bash
# Build PianoAuto's piano-rotated, native macOS's automatic display rotation agent (docs/drivers/sensors.md section 5a): the host
# tests first (package/PianoAuto/tests/run-host-tests.py: the rotation policy, the touch transform, the sensor streams, the ambient
# backlight), then the arm64 binary (ad-hoc signed by the linker) into out/piano-rotated with its launchd job and install script,
# ready for tooling/k27/stage-soft-metal.py --file (the staging recipe is in sensors.md section 5a). Prints the directory.
#   environment: CC / CXX (host compilers for the tests), MACOSX_MIN (deployment target, default 14.0)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
P=$D/package/PianoAuto; S=$D/package/PianoSensors; O=$D/out/piano-rotated
python3 "$P/tests/run-host-tests.py" >&2
mkdir -p "$O"
xcrun clang -arch arm64 -mmacosx-version-min="${MACOSX_MIN:-14.0}" -fobjc-arc -O2 -Wall -Wextra -Werror -I "$P" -I "$S" -I "$D/package/include" \
  "$P/daemon/piano-rotated.m" "$P/AutoRotate.c" "$S/SensorsPolicy.c" "$S/SensorsOrient.c" "$S/SensorsFixed.c" \
  -framework Foundation -framework CoreGraphics -framework IOKit -o "$O/piano-rotated"
cp "$P/daemon/com.unixnu.piano-rotated.plist" "$P/daemon/install-piano-rotated.sh" "$O/"
chmod 755 "$O/install-piano-rotated.sh"
plutil -lint "$O/com.unixnu.piano-rotated.plist" >&2
codesign -dvvv "$O/piano-rotated" 2>&1 | grep -E "^(Signature|CDHash)=" >&2 || true
echo "$O"
