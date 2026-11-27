# BH-2: macOS 27.3 powerd storage contract

**OWNER-0748(b), updated 2026-11-27. Host analysis only; fix-image gate HOLD.**
The fresh `v131y12u-daily1` error is **powerd's own missing-BatteryData return**,
before a persistent write. `serviceFlags=4` is its literal baseline battery-health
service-flags seed, not a storage-backend selector. The older `v131y12t-qual1`
`0xe00002ca` occurs later, after record construction, in the persistent writer.
Kernel CHRP synchronization independently returns `0xe00002d8` for its missing
controller. Equal numeric codes do not connect these different call paths.

Both `piano-power-ps-fields` and `piano-power-ps-health` remain default **0**.
The latter requires `piano-power-ps=1 piano-power-ps-fields=1`. The 80/75% charge
hold, fixed-PD policy and Charge Pump / MiPPS defaults are unchanged. No device
operation, image build, installation or remote push was performed for this work.

## Sources and address convention

These are independently written engineering notes, without copied instruction
bytes, binary payloads or runtime identity data. The local read-only inputs are
the supplied powerd disassembly, strings, symbol table and executable; the decoded
27.3 shared cache; the prior `batt-fields2` commits; and the saved qualification
logs. Input locations and extracted artifacts stay outside this repository.

`ipsw macho info --arch arm64e` reports minimum OS/SDK **27.3**, source version
**2043.60.6**, Mach-O UUID `29CB19CD-F120-31A5-A527-31D9529512E9` for powerd.
The supplied universal executable SHA-256 is
`045e9090396eeebb64e37b71f220c74d51d661fef3f072a14264519856685105`.
The shared cache is the supplied `26C5035r` input. These identifiers establish
the local analysis inputs, not an independent hash of the running tablet's file.

Powerd addresses below are **unslid arm64e virtual addresses**; subtract
`0x100000000` for image-relative offsets. Thus `0x100001ef0` is offset `+0x1ef0`.
Shared-cache IOKit addresses are unslid cache VAs. Reporter addresses are offsets
in the previously extracted 27.3 `SPPowerReporter` arm64e image. Its SHA-256 is
`c3f95a2ffb187599494fb9ae42d2bc585bc74ec0e0c8af1de98bd5bccbf144f7`.

The prior changes inspected were `96545f31`, `01a2606e`, `ad563440`, `3aa4cfbc`,
`6fa1e3fd`, `bb30ebb4` and `211e9e98`, together with their field-publisher
prerequisites `387747d1` and `429e8e67`. Only the relevant source and tests were
ported; previous analysis documents containing local paths were not copied.

## Saved failure evidence

The following table describes the **older health-enabled** `v131y12t-qual1`
capture. The new daily capture is analyzed separately below.

| Saved reference | Sanitized observation | Consequence |
|---|---|---|
| `bf-grade.txt`, `bf-pack.txt` | Exactly one registered `AppleSmartBatteryPack`, ID 0, FeatureFlags 1024; nested identity present; full/raw/nominal 8781 mAh, design 9200 mAh, cycles 122 | The earlier missing class/dictionary explanation no longer explains this failure. |
| `bf-grade.txt`, `bf-ioreg.txt` | Source MaxCapacity 100, CurrentCapacity 56; charge stop/resume 80/75, limit enabled | Pack absolute capacity and source normalized percentage have distinct contracts. |
| `bf-spp.txt` | Cycle Count 122; Condition Service Recommended; Maximum Capacity absent | Complete input telemetry did not establish healthy IOPS output. |
| `bh-powerd.txt` | Validation of absent stored identity returns status 0; new pack health data created; write then fails with `0xe00002ca`; update/evaluation report the same status repeatedly | Cold initialization reaches the persistent writer. A null **stored** identity is not evidence that BatteryData identity is missing. |
| `console.out:44–48` | NVRAM bank size 8192; CHRP common partition size `0x800`; no system partition | Finite proxy capacity is relevant, but the failing JSON's serialized size and remaining capacity were not captured. |
| `console.out:1130–1138` | No NVRAM controller; synchronization returns `0xe00002d8` | Durable NVRAM storage is missing. This sync failure alone cannot explain the observed write error, because powerd ignores sync's return. |

The saved health log has no `batteryHealthStoragePersistent` messages naming
the selected storage type and no raw `IORegistryEntrySetCFProperty` result.
The kernel console does not identify the failed battery-health write's size,
permission decision, or return code. These absences bound the diagnosis.

## Fresh daily evidence and exact NotReady path

Read directly: `20261127-runs/v131y12u-daily1/bh2-probe.txt` (876 lines) and
`console.out` (30,257 lines), supplied private host captures for `84d8bfdb`,
27.3 / `26C5035r`, with `piano-power-ps-fields=0 piano-power-ps-health=0`.
Raw captures and runtime identity data are not copied into this repository.

| Input | SHA-256 |
|---|---|
| `bh2-probe.txt` | `44541e3eab5fb62b7540175a6c8fac3d4098f5357e0ace26fc24e941cc1f69d8` |
| `console.out` | `597cf9d926bec7368fbdaf3fd0588ca72ce8c3a83971e80ff80c135a1a17c021` |
| `kernel-k273-0076-native.macho` | `00840bd590be173af429eb7a322534e6fa8a33b1ae9653a3218e5311eee76521` |

| Exact capture location | Observation | What it establishes |
|---|---|---|
| Probe line 12, class-instance counts | AppleSmartBattery=1, AppleSmartBatteryPack=0, IODTNVRAM=1, IONVRAMController=0 | An allocated NVRAM object does not establish the class of the DT path receiver. No opt-in pack exists in this capture. |
| Probe lines 321–327 | DT options is **IOPlatformDevice**; its only displayed property and the sole `nvram -p` entry are empty boot-args | The path used by powerd is a generic platform nub in this snapshot. It is not an IODTNVRAM receiver. |
| Probe lines 331–335, repeated through 875 | Validation status 0 with null stored serial; create status `0xe00002d8`, serviceFlags 4; reset/init/update errors follow | 109 occurrences of each message; zero Created new pack health data and zero Failed to write pack health data messages. The initialization failure precedes writing. Periodic sequences are about five seconds apart, with additional interleaved callbacks. |
| Console lines 44–49 | Bank 8192 bytes, common data offset `0x30`, size `0x800`, system offset `0xffffffff`, size 0; ephemeral mode unsupported | CHRP proxy was decoded. The missing system partition is not evidence of failure to initialize the format. |
| Console lines 1137–1149 | CHRP serializeVariables: No _nvramController; sync returns `0xe00002d8` | A separate controller-dependent sync fails. These lines do not identify a battery-health property write. |
| Console lines 1162–1165 | Image4 requires immutable NVRAM; allowed to mutate NVRAM: 0 | Preserve this posture. The message alone does not identify the return of the battery-health writer or prove a blanket denial of all IOKit NVRAM variables. |

### Literal origin of serviceFlags=4

In the exact supplied arm64e powerd, reset/create routine `sub_10001eaa0`
starts status at `0xe00002c2` (BadArgument) at `0x10001eacc–0x10001ead0`.
It calls `BHStorageSession resetForPack:` at `0x10001eaf8`, then checks the
source, source properties, logical pack and **cached pack BatteryData**:

1. `0x10001eb40` calls `sub_100012a80(packId)`. That accessor resolves the
   logical pack with `sub_1000123f4`, then returns its pointer at **`pack+0xa0`**
   (`0x100012aac`). The refresh path loads registry **BatteryData** into exactly
   that slot at `0x100017928–0x100017940`. Discovery separately matches the
   AppleSmartBatteryPack class and ID at `0x100016e40–0x100017050`.
2. `0x10001eb4c` branches on a null accessor result to `0x10001ebd8`.
   **`0x10001ebdc` adds `0x16` to `0xe00002c2`, producing `0xe00002d8`.**
3. `0x10001eb78` loads **literal 4** into w24. The cold log stub receives
   status=w22 and serviceFlags=x24 at `0x10001ec90–0x10001ec9c`, then
   `sub_1000696f0` emits Failed to create persistent storage
   (`0x10006971c`, log call `0x100069728`).
4. The same routine logs Failed to reset data for pack through
   `sub_100069738` (`0x10001ecac`), and returns status at `0x10001ebb4`.

**Meaning:** 4 is the baseline seed of powerd's **Battery Service Flags** word.
It has no SMC/NVRAM/CFPreferences selection meaning and is not read from an
IOService matching result, source FeatureFlags, options, or a controller.
Successful construction also seeds 4 (`0x10001ed24–0x10001ed30`), ORs in
additional conditions, and stores the word under Battery Service Flags at
`0x10001ee9c–0x10001eec0`. It therefore does not itself mean a missing controller
or a missing serial. Serial validation separately adds `0x400` for absent/wrong
type or `0x800` for empty at `sub_1000153c4`. The null-BatteryData exit precedes
that check. The stripped binary does not provide the private enum's source
spelling; no invented symbolic name or independent bit-2 fault is assigned here.

### Validation and propagation to the five messages

`sub_10001e43c` reads stored history through `sub_100015770` at `0x10001e4c8`.
It accepts a stored dictionary only with a nonempty Battery Serial and a Battery
Service Flags value. A successful read with nil history reaches the validation
error at `0x10001e794–0x10001e804`: the displayed serial is taken from the
**stored history**, and status is the read's w25=0. This does not report a
failed NVRAM set or a hardware serial read.

After the authentication gate (`0x10001e5ac`), it calls reset/create at
`0x10001e5c0`. The returned NotReady is copied to w19 at
`0x10001e5d4–0x10001e5d8`. The init error is emitted through
`sub_1000695fc` at `0x10001e82c` (format `0x100069620`, log `0x10006962c`).
The final update error uses `sub_10006963c`, called at `0x10001e6c0`, and
returns the same w19 at `0x10001e690`.

The write at **`0x10001e6d0` is reachable only when reset/create returns zero**.
The daily null-BatteryData exit never reaches it. It neither calls
nvramRawDataWrite nor obtains its NotReady from AppleSMC, a pack user client,
IODTNVRAM setProperty/setProperties, CHRP setVariable, or sync. Source properties
and a logical pack passed the earlier checks (otherwise status would remain
BadArgument); the failed prerequisite is specifically the cached BatteryData.
Zero live pack instances and both disabled publisher gates explain that missing
input without postulating a persistent-write attempt.

The **older** enabled run instead logs Created new pack health data and then
Failed to write pack health data. It passes reset/create and reaches
`sub_100015fd0`. Its `0xe00002ca` is the later normalized writer error described
below. The change in code is explained by reaching different stages with
different published inputs; it is not evidence that a storage error changed
from IOError to NotReady.

## End-to-end path table

`ours-OK` means implemented in the carried opt-in publisher, established by saved
evidence, or covered by the explicitly bounded host replay. It does not mean a
new RAM qualification passed. `ours-missing` identifies a demonstrated missing
capability. `unknown` identifies a missing observation or an unqualified path.

| Stage / check | Address or implementation | Status | Contract / evidence |
|---|---|---|---|
| Opt-in prerequisites | `PianoPower.cpp`, `readArgs` / `publishBattery` | ours-OK | Both arguments default 0; health requires fields and power source. No settings in a daily profile are enabled. |
| Measured fields | `Gauge.h`, `BatteryMap.h`, `BatteryHealthData.h` | ours-OK | Both gauges, valid cycles and design/full; partial/sentinel input withholds a new pack and retains the last complete health sample. |
| Singleton construction | `BatteryHealthSingleton shared` `0x10000f888`; `sharedSet:` `0x10000f900` | ours-OK | Associated-object singleton; sharedSet requires a non-null object and no previous instance. This is a userspace helper, not a required kernel service. |
| Persistent protocol | `BHPersistentStorageProtocol` Objective-C metadata | ours-OK | Required selectors are `dataCopyForPack:outData:` and `dataWrite:forPack:`. The published pack is not an implementation of this Objective-C protocol. |
| Persistent singleton | `sharedStorage` `0x100001b90`; `init` `0x100001c14` | ours-OK | Dispatch-once allocation, persistent log category; Mac storage initialization constructs the namespaced NVRAM key. |
| Pack count | `0x1000169a0–0x1000169c4` | ours-OK | Missing BatteryPackCount defaults to one logical pack. |
| Pack matching | `0x100016e40–0x100017050` | ours-OK | Match service **class** AppleSmartBatteryPack; numeric ID must equal pack index. Saved run has one, ID 0. |
| Pack telemetry | `0x1000170c0–0x10001724c` | ours-OK | BatteryData dictionary; string Serial inside it; RemainingCapacity, MaxCapacity, DesignCapacity, Voltage, AverageCurrent, CycleCount, NominalChargeCapacity. Capacities are absolute mAh. |
| Metric selection | `0x100017928–0x100017a18`, `0x10001a00c–0x10001a190` | ours-OK | Service-level FeatureFlags bit 10 selects nominal/design on Mac. A similarly named nested flag does not. |
| Update inputs | `0x100019104–0x1000191c4` | ours-OK | Positive AppleRawMaxCapacity and DesignCapacity, ratio in range; CycleCount present and not -1. MaxCapacity alone cannot supply the separate raw-full check. |
| Authentication | `0x10001dfa8`; `PackAuthManager` `0x10002ab70–0x10002ac94` | unknown | Unsupported auth result 2 is accepted; supported failed auth has its own flags. Saved creation implies this initialization gate passed then; full auth behavior is not qualified. No Apple auth data is fabricated. |
| Permanent failure | `0x1000176d4–0x1000176fc`, `0x10001e89c` | ours-OK | Read BatteryData PermanentFailureStatus; absent input clears cached value. A measured nonzero value sets flag `0x40000000`. Gauge fully-discharged is not PF. |
| History validation/init | `0x10001e43c–0x10001e8d0`; init `0x10001eaa0` | ours-OK | Read per-pack stored identity; absent/mismatched history needs initialization, valid telemetry and accepted auth. Saved log reaches new-data creation. |
| Internal write caller | `0x100015fd0–0x10001611c` | ours-OK | Validate pack dictionary and look up existing pack; call persistent singleton `dataWrite:forPack:`. Failure is logged and propagated, without a successful history publication. |
| Per-pack root shape | `dataCopyForPack:outData:` `0x100002934`; `dataWrite:forPack:` `0x100002b74` | ours-OK | Root `BatteryPacks` dictionary indexed by decimal ID string; per-pack dictionary. Read accepts legacy flat root. Write replaces one record and preserves other packs. Host replay covers this contract. |
| Backend selection | `storageTypeGet` `0x1000026dc`; predicate `0x10002a2c8` | unknown | Return `isMac XOR 1`; isMac compares MGCopyAnswer(DeviceClass) to Mac. Mac gives type 0 NVRAM; other/missing class gives type 1 CFPreferences. Runtime DeviceClass/type was not saved. |
| Root dispatch | write `0x1000026f8`; copy `0x1000027e8` | ours-OK | Type 0 NVRAM, type 1 CFPreferences, type 2 InternalError (`0xe00002c9`). No retry on the other backend after failure. |
| NVRAM encoding | `rootDictNVRAMWrite:` `0x100001f6c`; copy `0x100002034` | ours-OK | Foundation JSON bytes in NSData; decoded root must be a dictionary. Successful writes/copies cache the root in process memory. Invalid JSON and wrong types have separate errors. |
| NVRAM target | `init` `0x100001d0c–0x100001d58`; raw copy `0x100001de4`; raw write `0x100001e9c` | ours-missing | `IODeviceTree:/options`, key `40A0DDD2-77F8-4392-B4A3-1E7304206516:battery-health`. Daily receiver is IOPlatformDevice, whose property method is Unsupported; the old run's raw result remains unknown. The UUID is a public variable namespace, not a unit identity. |
| IOKit wrapper | cache `_IORegistryEntrySetCFProperty` `0x184eca620`; plural `0x184eca6a8` | ours-OK | Wrap one key/value in CFDictionary, IOCFSerialize, issue registry-set-properties MIG call; choose transport error if nonzero, otherwise return the kernel method's result. Does not itself map every error to IOError. |
| Kernel property method | local XNU `IOUserClient.cpp`, `is_io_registry_entry_set_properties`; `IONVRAM.cpp`, `IODTNVRAM::setProperties` | unknown | Unserialize, MAC policy / allowable-property checks, then setProperties and per-key setter. The failed call's actual result was not captured. |
| Kernel variable validation | `IODTNVRAM::setPropertyWithGUIDAndName` | unknown | Require initialized format; enforce variable permission, type, size; translate GUID, call format setVariable. Static candidates include NotPermitted/NotPrivileged/BadArgument/NoSpace/NotReady. The executable carries system-NVRAM allowance; runtime entitlement recognition was not measured. |
| CHRP bounds | local XNU `IONVRAMCHRPHandler::setVariableInternal` | unknown | Encoded new variable size + used − old size must fit its partition. The observed 2048-byte common partition is a bound, not proof that this health record exceeded it. |
| NVRAM synchronization | `0x100001f30–0x100001f4c`; CHRP `serializeVariables` | ours-missing | After successful property write, set string-valued IONVRAM-SYNCNOW-PROPERTY. Powerd ignores that return and reports success. Saved controller absence prevents durable sync. |
| File-backed alternative | CFPreferences methods `0x1000021fc–0x1000026d8` | unknown | Domain com.apple.batteryhealthdata; current user/current host; explicit current container `/var/MobileSoftwareUpdate/Hardware/Battery/`; fallback legacy read and successful-write legacy cleanup. Selected only for type 1, not as a Mac NVRAM fallback. |
| Volatile session | `BHStorageSession init` `0x100002f74`, get `0x1000030fc`, set `0x100003224` | ours-OK | batteryHealthStorageVolatile is a log category for an in-memory, locked dictionary keyed by pack/key. Session reset is not a durable backend or a persistent-write fallback. |
| Persistent tester read | handler `0x100015430`; worker `0x1000155d8`; pack read `0x100015770` | ours-OK | Require batteryTester entitlement, use power-management queue, iterate actual packs, return XPC data / returnCode. Not called in these host tests. |
| Persistent tester set/entries | handler `0x100016120`; worker `0x10001638c`; dispatch `0x100062cf4` | ours-OK | setBatteryHealthPersistentData dispatch reads setBatteryHealthPersistentDataEntries; require entitled caller and dictionary; merge entries into existing record, then use internal writer. Absent record returns NotReady. |
| Persistent tester delete | handler `0x100015b10`; worker `0x100015db4`; dispatch `0x100062cb0` | ours-OK | deleteBatteryHealthPersistentData reads nonempty deleteBatteryHealthPersistentDataKeys array; validate every string/key before mutation; remove requested fields and rewrite through the same writer. Does not delete the NVRAM variable. |
| Health/service evaluation | `0x100023ff8`, `0x10002441c` | ours-OK | Service state 0 is healthy, PF maps to state 8; invalid capacity flags can map to state 5. Private health normal is 0, distinct from legacy kernel IOPM Good=3. |
| IOPS public/private output | `0x1000189dc–0x100018b64`; cache `_IOPSGetBatteryHealthState` `0x184ef9ff8` | ours-OK | Per-pack IOPSBatteryHealthState and Battery Service State; XPC getBatteryHealthState returns IOPSPrivateBatteryHealthState. These are outputs, not invented registry inputs. |
| IOPS power-source health merge | `0x10001bf8c–0x10001c450` | unknown | Entitled power-source description merges persistent per-pack history into BatteryPacks[*].BatteryHealth. The saved failed writer never established the expected output. |
| System Information | reporter `+0x1608–+0x1738` | unknown | First active pack's BatteryHealth; service state exactly 0 => Normal; missing/nonzero => Service Recommended. Nonnegative Maximum Capacity Percent => clamped percentage; missing/negative or service state 8 => em dash. Candidate runtime remains unqualified. |
| Cross-boot persistence / UI / replacement | saved logs plus synthetic aggregate identity | ours-missing | No durable backend qualified; no successful UI result; synthetic logical identity cannot detect physical pack replacement. No previous history is cleared to hide this. |

SMC is involved in other power/pack-management work, but neither branch of
this persistent writer sends an SMC key write, opens AppleSmartBatteryPack as
a storage user client, or calls pack `setProperties`. Adding storage-looking
properties to the pack service cannot satisfy this writer. The actual Mac
storage receiver is the options node.

The arm64e Objective-C caller ABI for `dataCopyForPack:outData:` is x0=self,
x1=selector, x2=pack pointer, x3=output dictionary pointer; for
`dataWrite:forPack:` x2 is the dictionary and x3 the pack pointer. Both return
IOReturn in w0 and assert the power-management dispatch queue. Pack ID is read
from the pack structure at `+4` by `packIdForPack:` (`0x1000028fc`). The plain
internal writer at `0x100015fd0` receives logical ID in w0 and dictionary in x1.
It looks up a real pack through `0x1000123f4`, which rejects ID >= pack count
and also requires a matching stored ID, before calling the Objective-C writer.
Missing history is success with a nil output; it is distinct from a failed
read. Public tester endpoints do not expose a generic unrestricted storage API.

## Older IOError: exact writer conditions and evidence boundary

On the **NVRAM branch**, the failing path is:

1. `0x10001e6d0` (new initialization) or `0x10001e8d0` (updated history)
   calls `0x100015fd0`.
2. `0x100016040` calls `dataWrite:forPack:`; `0x100002d9c` calls
   `rootDictWrite:`; type 0 dispatches at `0x100002744` to rootDictNVRAMWrite.
3. JSON encoding succeeds; `0x100001fbc` calls nvramRawDataWrite.
4. The raw writer initializes its result to **`0xe00002ca` at
   `0x100001eb4–0x100001eb8`**. It finds options and calls
   **IORegistryEntrySetCFProperty at `0x100001ef0`**.
5. **At `0x100001ef4`, a nonzero result takes the failure path and leaves
   `0xe00002ca` unchanged.** The original result is discarded. It returns at
   `0x100001f58`; wrappers propagate it to the saved Failed to write pack health
   data log at `0x1000160bc–0x100016104`.

If options lookup instead returns zero at `0x100001ee0`, the error is changed
to **`0xe00002f0` (NotFound)** at `0x100001f04`. If the property write succeeds,
the later sync call at `0x100001f40` is ignored and the result is zeroed at
`0x100001f4c`. Therefore **controller absence cannot directly cause the saved
`0xe00002ca` through this raw writer**. It is a separate, demonstrated durability
gap. A controller alone is not an evidence-backed fix for the observed failure.

On the **CFPreferences branch**, `0x100002774` selects rootDictCFPrefsWrite.
At `0x10000253c`, `_CFPreferencesSynchronizeWithContainer` returning false
selects `0xe00002bd + 0xd = 0xe00002ca` at `0x100002540–0x100002548`.
This branch can produce **the identical saved code** without a NVRAM property
write. The current log does not distinguish it. Assuming that an Apple-looking
model name proves MG DeviceClass=Mac is insufficient.

**Older-run root cause established:** the new telemetry passes discovery/initial creation,
but persistent-write success is a required dependency and fails. **Exact kernel
leaf / backend: unknown.** The strongest conditional location is the masked
NVRAM property failure above; the alternative CFPreferences synchronization
failure must remain open until the backend is identified. No existing log proves
partition exhaustion, missing entitlement, an immutable-variable policy, failed
serialization, or a particular kernel setter return.

The minimal additional evidence is the runtime DeviceClass/storage type and
the failed operation's **raw** return (plus serialized length and partition usage
if it is NoSpace), captured before powerd normalizes it. It could come from an
existing more complete capture or a separately qualified diagnostic build.
This task performs no new capture on the tablet. Do not disable entitlement
checks, expand global NVRAM permissions, patch the daemon, select a false device
class or fabricate UI fields to work around an unidentified error.

## Kernel 0076: receiver, setter and synchronization

These are direct disassembly findings in the supplied, hashed **0076** kernel,
using its symbol table. Kernel addresses are unslid full VAs. Public
[IONVRAM.cpp](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/Kernel/IONVRAM.cpp)
and
[IONVRAMCHRPHandler.cpp](https://github.com/apple-oss-distributions/xnu/blob/main/iokit/Kernel/IONVRAMCHRPHandler.cpp)
provide readable context for the same division between property validation,
in-memory variable updates and controller-dependent serialization. The public
sources are not substituted for the 0076 binary.

### The live options receiver matters before the CHRP path

The cache's IOKit wrapper was re-disassembled directly from the supplied 27.3
dyld cache: `_IORegistryEntrySetCFProperty` at `0x184eca620` creates the
single-entry CFDictionary and calls the plural method at `0x184eca678`.
The plural method serializes, calls MIG at `0x184eca704`, then selects transport
error or kernel result at **`0x184eca714–0x184eca71c`**. The IOError masking
belongs to powerd's raw writer, not this framework.

0076 `_is_io_registry_entry_set_properties` starts at `0xfffffff00c224064`.
It checks the decoded object, MAC policy (`0xfffffff00c224160`) and allowable
properties before the virtual **setProperties** call at `0xfffffff00c224368`.
This is an IOReturn method, not the boolean setProperty overload.

The live **IOPlatformDevice** vtable (`0xfffffff007e63198`) has the inherited
setProperties slot at table+`0x330` (object vptr+`0x320`). Its chained pointer
resolves to **IORegistryEntry::setProperties**, `0xfffffff00c1967b4`, which
returns literal **`0xe00002c7` (Unsupported)**. The equivalent IODTNVRAM
vtable slot (`0xfffffff007e66590+0x330`) resolves to `0xfffffff00c217c24`.
The class name in the fresh probe is therefore a functional distinction.

**Conditional consequence:** on the Mac writer, with the daily receiver and
successful transport/serialization/MAC checks, the battery-health property set
would return Unsupported and powerd would normalize it to IOError. It would
never reach IODTNVRAM or CHRP. The current daily initialization stops earlier;
this is not a captured daily write return and does not retroactively prove the
older receiver/backend. An earlier policy denial could also mask the receiver's
return. `IODTNVRAM=1` in an allocation census cannot overcome this routing gap.

### Verified platform publication site (`ArmDTPlatform::excludeList`)

A read-only live registry inspection on installed `v13.1y12q` (2026-11-27,
`ioreg -r -c IODTNVRAM -l -w0` and `ioreg -p IODeviceTree -n options -w0`)
together with source inspection of XNU `IOPlatformExpert.cpp` and
`ArmDTPlatform.cpp` (pinned commit `e8bd8d5d`) establishes the exact site where
the duplicate `IOPlatformDevice` nub replaces `IODTNVRAM`:

1. In `IODTPlatformExpert::processTopLevel(rootEntry)` (`IOPlatformExpert.cpp:1737`),
   `publishNVRAM()` creates the real `IODTNVRAM` object (`gIOOptionsEntry`,
   observed live in `IOService` as `options <class IODTNVRAM, id 0x10000011a>`
   with active child `IODTNVRAMDiags` reporting `CommonUsed = 11` bytes out of
   `0x800`, `SystemUsed = 0`, `Version = 1`, and child `options-common
   <class IODTNVRAMVariables>`).
2. Twelve lines later (`IOPlatformExpert.cpp:1749`), `processTopLevel()` runs
   `createNubs(this, IODTFindMatchingEntries(rootEntry, kIODTExclusive, excludeList()))`.
3. `ArmDTPlatform::excludeList()` returns `"()"` (empty list; compare
   `GenericARMPE::excludeList()` which explicitly includes `'options'`).
4. Consequently `createNubs()` instantiates an `IOPlatformDevice` nub for every
   top-level device-tree child including `options` (observed live 49 objects
   later as `options <class IOPlatformDevice, id 0x10000014b>` under
   `IODeviceTree:/`), shadowing `IODTNVRAM` (`0x10000011a`) at
   `IODeviceTree:/options`.
5. The same live check also confirmed `/var/MobileSoftwareUpdate/Hardware/Battery/`
   is absent and `hw.model = Mac17,11` / `hw.targettype = K114c`.

Excluding `'options'` in `ArmDTPlatform::excludeList()` (returning `"('options')"`
while keeping `chosen`, `cpus`, `arm-io`, `defaults`, `product`, and `buttons`
unchanged) is therefore the exact, minimal single-site edit needed to preserve
the canonical `IODTNVRAM` receiver at `IODeviceTree:/options`. Because
`ArmDTPlatform` is linked into the kernel binary (`stage_platform` in
`tooling/ci/k27-kernel.sh`), shipping it requires a dedicated kernel build and
separate RAM qualification (not mixed into the `v13.1y12u` ESP-only daily
update on kernel `0076`). Durable cross-reboot NVRAM persistence remains a
separate follow-up requirement (`IONVRAMController`).

### If the receiver is the actual IODTNVRAM

| Function | 0076 address | Verified behavior relevant here |
|---|---|---|
| IODTNVRAM::setProperties | `0xfffffff00c217c24` | Iterates dictionary entries; calls setPropertyInternal at `0xfffffff00c217cfc`, propagating the first nonzero IOReturn. |
| IODTNVRAM::setProperty | `0xfffffff00c217a2c` | Calls setPropertyInternal at `0xfffffff00c217a38`; converts success to bool. It is not the MIG setter's result-bearing entry. |
| IODTNVRAM::setPropertyInternal | `0xfffffff00c2178a4` | Parses GUID/name; calls setPropertyWithGUIDAndName at `0xfffffff00c2179f8`. |
| IODTNVRAM::setPropertyWithGUIDAndName | `0xfffffff00c20ae14` | Checks format at `0xfffffff00c20ae8c`; missing format returns NotReady at `0xfffffff00c20b8a4–0xfffffff00c20b8b0`. Ordinary data writes check permission/type/size, translate GUID at `0xfffffff00c20b7c4`, then call format setVariable at `0xfffffff00c20b7f0`. |
| IONVRAMCHRPHandler::setVariable | `0xfffffff00c20fc00` | System-active query and GUID selection; locks variable dictionary; calls setVariableInternal at `0xfffffff00c20fd74`. **No serializeVariables/sync/controller check.** |
| IONVRAMCHRPHandler::setVariableInternal | `0xfffffff00c20dc98` | Computes encoded replacement delta against selected partition usage/size. Common exhaustion returns NoSpace (`0xe00002db`) at `0xfffffff00c20de04`; system exhaustion at `0xfffffff00c20ded4`. Successful dictionary mutation marks new data; no controller required. |
| IODTNVRAM::syncInternal(bool) | `0xfffffff00c216284` | Checks safe context/format; normal sync-now is rate limited. Calls format sync at `0xfffffff00c21640c`, preserves its IOReturn. |
| IONVRAMCHRPHandler::sync | `0xfffffff00c20feb0` | Calls serializeVariables at `0xfffffff00c20fef0`; logs nonzero result and returns it at `0xfffffff00c20ff14`. |
| IONVRAMCHRPHandler::serializeVariables | `0xfffffff00c20ee54` | Controller pointer at this+`0x18`; null test `0xfffffff00c20eec0–0xfffffff00c20eec4`, also after the lock assertion at `0xfffffff00c20f7ac`. Null branch logs No _nvramController at `0xfffffff00c20f7d4`; constructs `0xe00002bd + 0x1b = 0xe00002d8` at `0xfffffff00c20f7dc–0xfffffff00c20f7e4`. |
| IODTNVRAM::registerNVRAMController | `0xfffffff00c2160fc` | If format exists, delegates to format setController at `0xfffffff00c216198`. Does not create a controller from proxy data. |
| IONVRAMCHRPHandler::setController | `0xfffffff00c20fdd0` | Installs controller if absent, then reloads backing data. Controller registration is not a harmless flag toggle. |

Only the special string-valued `IONVRAM-SYNCNOW-PROPERTY` /
`IONVRAM-FORCESYNCNOW-PROPERTY` branches call syncInternal
(`0xfffffff00c20b1d4`). The ordinary battery-health NSData branch does not.
Consequently the proposed chain **battery-health set → setVariable → sync →
No _nvramController → powerd NotReady** does not exist here. Even a raw
NotReady from a property failure would be masked to IOError by powerd; an
error from the subsequent explicit sync is discarded.

The observed initialized CHRP format rules out its missing-format branch for
that object at initialization time. The Image4 immutable-NVRAM log remains
separate evidence of security posture; neither its code nor its policy should
be changed to make this experiment pass.

## ADT provenance and rejected partition-only repair

[make-piano-adt.py](../../tooling/adt/make-piano-adt.py) imports
`unixnu.devicetree.generate` from the local research UniXNU checkout's
`scripts/` directory. That generator's `empty_nvram_proxy()` supplies
chosen's four NVRAM properties. The inspected sheng
`scripts/make-sheng-adt.py::empty_nvram_proxy()` independently has the same
layout and explicitly describes it as volatile, without a controller:

| Bank region | Header offset | Total / payload size |
|---|---|---|
| nvram | `0x0` | Apple header `0x20` |
| common | `0x20` | `0x810` including header; data `0x30..0x82f` (`0x800` bytes) |
| free | `0x830` | Remaining `0x17d0` bytes including header |

Bank size is `0x2000`, count 1, current bank 0. Each CHRP header has an
end-around checksum; the Apple header carries Adler-32 of bytes `[20:]`.
There is no system partition, persistent backing file, or controller in this
generator. Repo `tooling/adt/`, `package/`, and `patches/` contain no NVRAM
controller/storage implementation; the NVRAM references in kernel patch 0068
only trace halt-time sync. The inspected sheng package/patch sources likewise
provide no controller implementation.

Crucially, **0076 already handles an absent system partition**.
CHRP setVariable queries getSystemPartitionActive; with false it maps the
Apple system GUID to the common GUID (`0xfffffff00c20fcd0`,
`0xfffffff00c20fd3c–0xfffffff00c20fd48`). With a system region it selects that
region (`0xfffffff00c20fd30`). Partition absence is therefore not an automatic
NoSpace/NotReady error. Adding system changes routing, available capacity and
system-variable policy context; it supplies neither missing BatteryData, a
correct options receiver, nor persistence. It is not the selected fix.

A RAM-only controller could let sync report success but would still lose
history across reboot. Registering it can reload/merge its own bank image;
blank backing, false successful durability and interactions with Image4's
global NVRAM consumer make it unsuitable as a battery-only repair. Real storage
requires an owned durable medium, crash-consistent updates and boot reload,
bounded writes/wear, and unchanged variable permissions and immutable security
state. No existing implementation/evidence here justifies those guarantees.

## What Normal and Maximum Capacity require

The source's numeric BatteryHealth=3 is the kernel IOPM quality enum. It is not
the private health enum and does not select Normal in the 27.3 reporter. The
reporter reads IOPS output, not a similarly named top-level registry property.

For the saved healthy 122-cycle fixture, the Mac nominal metric path computes
`ceil(100 * 8781 / 9200) = 96`. The initializer uses 104 below 21 cycles; later
history can differ from the instantaneous ratio, and the UI clamps at 100.
Updates require the separate raw-full field and can lower nominal history by
one point after five further cycles. Authentication, service flags, valid
identity, persistent write/readback and successful IOPS merge must also pass.
PF status must remain truthful; it deliberately produces Service Recommended
and hides maximum capacity. Absence of a measured PF value is not a fabricated
Apple authentication or diagnostic result.

The carried [publisher](../../package/PianoPower/BatteryHealthData.h) supplies
measured pack inputs and FeatureFlags, and the
[IOService adapter](../../package/PianoPower/PianoBattery.cpp) registers the pack
before the source's discovery/update. It supplies no precomputed maximum or
condition. This repairs the earlier input gap and reproduces the failed run's
pack shape; **it does not repair the persistent backend**.

## Host regression scope and validation

[run-battery-health-host.sh](../../package/PianoPower/tests/run-battery-health-host.sh)
runs both replays with **ASan and UBSan**:

- [battery_health_host.cpp](../../package/PianoPower/tests/battery_health_host.cpp)
  serializes the actual shared publisher schema through CF property lists;
  checks class/ID, metric placement, all required capacities, cycles, PF,
  signed current, transient invalid telemetry, numeric allocation failures and
  off gates. Its healthy UI calculation explicitly assumes working storage.
- [battery_storage_host.mm](../../package/PianoPower/tests/battery_storage_host.mm)
  uses real Foundation JSON and disposable file storage with injected provider
  errors. It checks cold reads, legacy shape, malformed JSON/type rejection,
  full replacement, entry merge, other-pack preservation, decimal ID boundaries,
  delete validation before mutation, empty records, exact size-boundary behavior,
  failed-write cache preservation, new-instance reads and simulated reboot.
  Permission/space/not-ready/unsupported/allocation failures all reproduce the
  same normalized IOError. It also proves that an ignored sync failure can
  return success while losing data on reboot, and that a CFPreferences sync
  failure can yield the same error as the NVRAM branch.

The provider limit is a test parameter, not an invented powerd JSON bound.
The decimal ID boundary test covers internal key formatting; it does not claim
UINT32_MAX is a real pack. The actual caller looks up a known pack before using
its pointer, and the kernel independently bounds its serialized IOKit message.
Delete removes fields inside stored JSON; generic deletion of the battery-health
NVRAM variable is forbidden by XNU's variable policy. Tester XPC calls require
`com.apple.private.iokit.batteryTester` and are not executed by these tests.

These are clean-room **contract models**, not execution of Apple powerd,
IODTNVRAM, MAC policy, the real CFPreferences container, or System Information.
Temporary files prove the replay provider can persist, not that the product
has a persistent backend. No test success is a RAM/UI qualification.

Prior publisher-port validation (2026-11-27, retained for provenance):

| Command | Result |
|---|---|
| `python3 scripts/ci/self-tests.py --only package/PianoPower/tests/` | **PASS:** 8 passed, 0 failed, 2 suite-owned skips. The runner also reports 19 existing C/C++ sources without standalone build lines; enclosing charge/platform suites and the native build run their host tests. |
| `bash tooling/k27/build-native-piano-power.sh` | **PASS:** host tests including both ASan/UBSan health replays; 23 native arm64e objects; import check passes (404 kernel imports, 7 library imports). No image built. |
| `python3 scripts/ci/test-doc-links.py` | **PASS:** 173 Markdown files, 61/61 driver index entries, 0 errors. |
| `python3 scripts/ci/privacy-scan.py` | **PASS:** 0 hits, 0 refused files. Staged additions and the full branch diff also pass with 0/0. |
| `git diff --check` | **PASS.** |

Current root-cause branch validation (2026-11-27):

| Command / check | Result |
|---|---|
| `python3 scripts/ci/self-tests.py --only package/PianoPower/ --only tooling/adt/` | **PASS:** 12 passed, 0 failed, 2 suite-owned skips, 19 existing C/C++ sources without standalone build lines. Includes charge/platform suites, ASan/UBSan health/storage replays and all four ADT tests. |
| Fresh disassembly | **PASS:** original powerd SHA matches; 460 history/init, 337 reset/create and 95 cold-log instructions freshly decoded from arm64e match the supplied disassembly. Required 0076 setter/CHRP/controller routines and receiver vtable inspected; IOKit property wrapper inspected directly in the supplied cache. Symbol caches/disassemblies are temporary host artifacts, not repository payloads. |
| `python3 scripts/ci/test-doc-links.py` | **PASS:** 174 Markdown files, 2217 relative links, 462 heading references, 61/61 driver index entries; 0 errors. |
| `python3 scripts/ci/privacy-scan.py` | **PASS:** 0 hits, 0 refused files. |
| `git diff --check` | **PASS.** |

No production code, ADT, kernel patch, boot configuration or test model was
changed by this root-cause analysis. Existing contract tests already cover raw
Unsupported/NotReady normalization, ignored sync failures and reboot loss.
Passing them is not qualification of a real backend or a storage repair.

## Fix-image gate

**Status: HOLD — no verified single storage repair, no fix image.** A partition
edit or controller stub does not directly solve the daily error-producing call.

**Path:** Daily: source → logical pack → absent cached BatteryData →
`sub_10001eaa0` returns local NotReady → no persistent write. Enabled historical
path: measured pack → reset/create success → `sub_100015fd0` →
dataWrite:forPack → rootDictWrite → runtime backend → write/readback →
BatteryPacks[0].BatteryHealth → System Information. On the Mac branch, correct
options routing and a durable backend are separate requirements.

**Checks:** Fresh gates-off absence and local error origin are established;
older gates-on construction/write failure is established. The daily receiver's
Unsupported implementation and CHRP's missing-controller sync error are verified
in 0076. Host tests pass within the bounds above. The older selected backend/raw
return, a correctly routed permitted write, encoded size/partition usage and
durable reboot history remain unqualified. Required later checks are actual
storageType/MG DeviceClass, receiver class and identity at the write, raw IOKit
status, unchanged MAC/entitlement decisions, write/readback and a true reboot
readback. Then check Normal/maximum, truthful faults and pack-replacement
identity limitations. Preserve source MaxCapacity=100, percentage, charging/AC
state, 80/75 hold, fixed-PD policy and unknown time on AC idle/full/hold.

**Root cause:** The daily `0xe00002d8, serviceFlags=4` is missing cached pack
BatteryData with baseline service flags, expected with the publisher opt-ins
off. It is not the kernel sync return. The earlier `0xe00002ca` is a later
persistent-writer failure whose raw cause/backend was erased or not captured.
The daily Mac write target would additionally dispatch to IOPlatformDevice's
Unsupported method; even correcting that route leaves missing durability.
No evidence supports missing system partition as the unique root cause.

**ONE change:** **Design verified (`ArmDTPlatform::excludeList()` -> `"('options')"`):
preserve the existing IODTNVRAM object (`0x10000011a`) as the canonical
`IODeviceTree:/options` receiver during platform publication**, so
`IODTPlatformExpert::processTopLevel()` (`IOPlatformExpert.cpp:1749`) stops
creating the duplicate `IOPlatformDevice` nub (`0x10000014b`) that shadows it.
Retain `IODTNVRAM`'s original validation/`setProperties` methods and security
policies; keep `chosen`/`cpus`/`arm-io`/`defaults`/`product`/`buttons`
unchanged. Do not implement a forwarding bypass or generic writable options
dictionary. Because `ArmDTPlatform` is linked into the kernel binary
(`tooling/ci/k27-kernel.sh` `stage_platform`), shipping this requires a
dedicated kernel build and RAM qualification separate from the `v13.1y12u`
kernel-`0076` daily line; no kernel/platform patch is committed here yet.

For that one proposed change, the host review gate must prove a single canonical
IODTNVRAM path, no duplicate/replacement on repeated nub publication, unchanged
variable and MAC checks, unchanged boot-args/security variables, and clean
gate-off behavior. Kernel/platform execution and crash/reboot durability cannot
be established by the existing Foundation replay. If the historical backend is
CFPreferences, this route repair does not address it. A persistent controller
would be a separate design with the storage/security tradeoffs above, not an
addition hidden inside this change. This is why no safe complete fix image can
be selected from the present evidence.

Both `piano-power-ps-fields` and **BH-2 `piano-power-ps-health` stay default 0**;
existing health opt-in still requires fields and source. No new enablement is
made merely to remove the daily log. Rollback remains omission/0. **No SIP/CSR
changes, no Image4/security weakening, no history deletion; 80/75 hold and Charge
Pump OFF remain untouched.** No tablet, queue, frozen output, login secret,
publish or push operation is authorized/performed by this work.
