#!/bin/bash
# NativeRamBlock (the Data disk) for native piano: as macOS-DroidVM's scripts/build-native-ram-block.py (publish once IOResources has
# matched IOBSD), but WITHOUT the VM's DroidVMDataStore.c: the changed pages go to NativeUfs's Data store window at the end of
# linux_rootfs (native_ufs_store_*, boot arg data-persist=2). The KC linker takes no weak imports, so the store hooks are plain imports
# resolved against NativeUfs (tooling/k27/kext/NativeRamBlock.json lists it as a library).
set -euo pipefail
D=$(cd "$(dirname "$0")/../.." && pwd)
K=${K27_TREE:-$HOME/Downloads/XiaomiPad8Pro-Ubuntu/droidvm-build/macOS-DroidVM}
SRC=$K/sources/UniXNU/drivers/NativeRamBlock
cd "$K"
STAGE=$(mktemp -d); trap 'rm -rf "$STAGE"' EXIT
python3 - "$SRC/NativeRamBlock.cpp" "$STAGE/NativeRamBlock.cpp" <<'PY'
import sys, re
text = open(sys.argv[1]).read()
old = '  registerService();\n  return true;\n'
start = 'bool NativeRamBlock::start(IOService *provider)\n'
assert text.count(old) == 1 and text.count(start) == 1, 'the end of start() changed'
publish = '''#include <kern/thread.h>
// UniXNU-piano build-native-ram-block.sh (as macOS-DroidVM build-native-ram-block.py): publish once IOResources has matched IOBSD
static void publishWhenBSDMatched(void *argument, wait_result_t)
{
  NativeRamBlock *ram = static_cast<NativeRamBlock *>(argument);
  if (OSDictionary *matching = IOService::resourceMatching(kIOBSDKey)) {
    if (IOService *resources = IOService::waitForMatchingService(matching)) resources->release();
    matching->release();
  }
  ram->registerService();
  ram->attachStore();
  ram->release();
}

'''
text = text.replace(start, publish + start).replace(old, '''  retain();
  thread_t thread;
  if (kernel_thread_start(publishWhenBSDMatched, this, &thread) != KERN_SUCCESS) {
    release();
    return false;
  }
  thread_deallocate(thread);
  return true;
''')
old_end = "    IOLockUnlock(gate);\n  }\n\n  bool available()"
assert text.count(old_end) == 1, 'the end of attachStore() changed'
text = text.replace(old_end, """    IOLog("NativeRamBlock: Data store state %u (1 blank, 2 resumed, 3 fresh, 4 failed), header %u, first LBA %llu, %llu pages, direct %d\\n",
          native_data_persist_state, native_data_persist_header, native_data_persist_first_lba, native_data_persist_pages, direct);
    IOLockUnlock(gate);
  }

  bool available()""")
n = len(re.findall(r'(native_ufs_store_\w+\([^)]*\)) __attribute__\(\(weak_import\)\);', text))
assert n == 6, f'{n} weak store hooks'
text = re.sub(r'(native_ufs_store_\w+\([^)]*\)) __attribute__\(\(weak_import\)\);', r'\1;', text)
open(sys.argv[2], 'w').write(text)
PY
python3 scripts/build-native-kext.py NativeRamBlockUfs "$STAGE/NativeRamBlock.cpp" overlay/kext/NativeKextGlue.cpp \
  -I "$SRC" -I "$K/out/x12377/storage-abi/include" -I "$K/out/x12377/xnu-m/osfmk" \
  -D U:TARGET_OS_OSX -D TARGET_OS_OSX=1 --obj "${K27_OBJ:-k27-native-obj}"
