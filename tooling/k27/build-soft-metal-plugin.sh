#!/bin/bash
# Build package/NativeSoftMetal (the Metal plugin of NativeSoftGpu: a device without a GPU, so WindowServer composites in software) as
# out/soft-metal/NativeSoftMetal.bundle, macOS layout, arm64e, signed ad hoc. tooling/k27/stage-soft-metal.py puts it into a Data seed and
# its CDHash into a trust cache.
#   environment: DEVELOPER_DIR (default /Applications/Xcode.app/Contents/Developer, whose macOS SDK has Metal's headers)
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}
SDK=$(xcrun --sdk macosx --show-sdk-path)
P=$D/package/NativeSoftMetal
O=$D/out/soft-metal
B=$O/NativeSoftMetal.bundle/Contents
rm -rf "$O/NativeSoftMetal.bundle"
mkdir -p "$B/MacOS"
xcrun clang++ -target arm64e-apple-macos14.0 -isysroot "$SDK" -fobjc-arc -O2 -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter \
  -c -x objective-c++ "$P/NativeSoftMetalPlugin.mm" -o "$O/NativeSoftMetalPlugin.o"
xcrun ld -arch arm64e -platform_version macos 14.0 27.0 -syslibroot "$SDK" -F "$P/frameworks" -bundle -dead_strip \
  -o "$B/MacOS/NativeSoftMetal" "$O/NativeSoftMetalPlugin.o" \
  -framework Foundation -framework Metal -framework IOKit -framework CoreFoundation -framework IOGPU -lobjc -lc++ -lSystem
python3 - "$B/Info.plist" <<'EOF'
import plistlib, sys
open(sys.argv[1], 'wb').write(plistlib.dumps(dict(
    CFBundleIdentifier='com.unixnu.NativeSoftMetal', CFBundleExecutable='NativeSoftMetal', CFBundlePackageType='BNDL',
    CFBundleName='NativeSoftMetal', CFBundleVersion='1', CFBundleShortVersionString='1', NSPrincipalClass='NativeSoftMetalDevice',
    CFBundleSupportedPlatforms=['MacOSX'], LSMinimumSystemVersion='14.0'), fmt=plistlib.FMT_XML))
EOF
codesign -f -s - "$B/MacOS/NativeSoftMetal"
codesign -v "$B/MacOS/NativeSoftMetal"
codesign -dv "$B/MacOS/NativeSoftMetal" 2>&1 | grep -E "^CDHash=|^Hash type|flags" || true
echo "$O/NativeSoftMetal.bundle"
