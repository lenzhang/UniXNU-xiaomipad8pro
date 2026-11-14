# The macOS Wi-Fi menu: PianoWifi under Apple's Wi-Fi stack (design, 2026-11-14)

Status: **M1 built and host-tested (2026-11-14, §5), not run on the tablet yet**: probe `ram-k273-wm1` (§5.8). The link itself works since boot 10i (2026-11-14: WPA2, DHCP, the internet over
`PianoWifiEthernet`, an Ethernet-like interface, with our own supplicant; [wifi.md](wifi.md) §8.8, §13). The owner's next step:
the Wi-Fi menu, Settings › Wi-Fi and CoreWLAN, through airportd and IO80211Family, as on a Mac.

## 1. What has to change

Today macOS sees an Ethernet service: no Wi-Fi menu, no network list, joining is a root-only profile file
(`/private/var/db/piano-wifi/profile`) read by the kext and by `piano-wifi-supplicant`. Apple's stack instead expects:

- an **IO80211 interface** (the Skywalk-era `IO80211SkywalkInterface`/`IO80211InfraInterface` family in macOS 27, no public headers)
  that airportd finds, powers and drives through the private **apple80211** requests (scan, join, state, RSSI, keys);
- **airportd** owning the networks (known networks, auto-join, the passwords in the keychain) and **eapolclient** running the WPA2
  four-way handshake in user space, handing the keys back to the driver;
- the menu extra and Settings reading airportd's state through CoreWLAN.

So the station logic we already have (scan, join, the data path, key installation: levels 6-8) stays; what changes is who asks for
it (airportd instead of our profile file) and through which interface (apple80211 requests instead of our 802.1X kernel control).

## 2. Plan (each phase verifiable in one boot, our Ethernet path as the control)

| Phase | What | Gate on the tablet |
|---|---|---|
| W0 | Facts: IO80211Family in our 27.3 KC (classes, sizes, vtables, which virtuals Apple's own `AppleBCMWLANCore` overrides), the apple80211 request numbers airportd and CoreWLAN use on 27.3, how EAPOL reaches eapolclient, prior art (AirportItlwm's reconstructed headers, sheng's binding) and its licences | host only (research report `private/host-checks/20261114-io80211/`) |
| W1 | A minimal IO80211 controller + interface beside `PianoWifiEthernet` (a switch, default off): registers, answers the card capabilities, power, MAC, driver/hardware versions | `ifconfig` shows the Wi-Fi interface, `networksetup -listallhardwareports` lists "Wi-Fi", the menu extra shows Wi-Fi: On |
| W2 | Scan through apple80211 (our level 6/7 scan list as the result) | the Wi-Fi menu lists the networks; `wdutil`/CoreWLAN scan returns them |
| W3 | Join through apple80211 (associate with the SSID/BSSID/RSN element airportd gives), EAPOL to eapolclient, keys from it (CIPHER_KEY) into our key installation, the port authorized | choosing the network in the menu with its password joins; DHCP; the menu shows the bars |
| W4 | Known networks and auto-join at boot (airportd's), roaming between the same SSID's access points (our rejoin), sleep/wake (10k) | after a reboot the menu joins by itself; Settings › Wi-Fi works |

The Ethernet path (`PianoWifiEthernet`, the supplicant, the profile file) stays the default until W3 passes and remains a fallback
switch afterwards.

## 3. Open questions (answered by W0)

1. The 27.3 control path between airportd and the driver: ioctls on the BSD interface (SIOCSA80211/SIOCGA80211), a Skywalk
   channel, or an IOUserClient; and how much of IO80211Family's own state machine a driver must implement.
2. Whether a subclass in our KC can override the Skywalk interface's virtuals safely (arm64e PAC, vtable layout), as our xHCI
   subclass plan (B7 path A2) needs too.
3. Whether eapolclient needs the Apple driver's PMK/PTK offload paths or works with plain EAPOL frames and CIPHER_KEY.
4. What the menu needs at minimum to show the interface at all.

## 4. W0's answers and the decision (2026-11-14 17:40)

From the host research (`private/host-checks/20261114-io80211/REPORT.md`, static analysis of the 27.3 KC, airportd, IO80211.framework
and eapolclient; not yet tablet-confirmed):

- airportd's control plane is **BSD ioctls on enX**: `SIOCSA80211` `0x802869c8` / `SIOCGA80211` `0xc02869c9` with the 40-byte
  `apple80211req` (name, type, value, length, pointer). The user-space IO80211 library prefers an IOUserClient but falls back to
  the ioctls. The core request numbers are unchanged from earlier releases (SSID 1, CIPHER_KEY 3, BSSID 9, SCAN_REQ 10,
  SCAN_RESULT 11, CARD_CAPABILITIES 12, STATE 13, POWER 19, ASSOCIATE 20, STATUS_DEV_NAME 23, DRIVER_VERSION 43, ...);
  `HE_CAPABILITY` moved to 570. The association request is version 1, **656 bytes**; scan results carry the elements inline at
  0x8c.
- **WPA2 runs in user space**: eapolclient does the four-way handshake over an `AF_NDRV` socket bound to enX (EtherType 0x888e)
  and installs the keys with `CIPHER_KEY` (148 bytes). The driver associates and programs keys; it does not run the handshake.
- airportd is launched on an `IO80211Controller` match; it lists interfaces from `getifaddrs` whose IOKit service carries
  `IO80211InterfaceRole = "Infrastructure"` and whose `SIOCGIFTYPE` is type 6, subtype 3 (Wi-Fi), and it checks DRIVER_VERSION,
  CARD_CAPABILITIES and STATUS_DEV_NAME first.
- Subclassing Apple's Skywalk classes is the expensive road: IO80211Family is not in our native collection, and
  `IO80211InfraProtocol` has 207 pure virtuals whose 27.3 signatures no symbol in our KC gives.

**Decision: the narrow binding first.** Our own `IOEthernetInterface` (PianoWifiEthernet's) answers the apple80211 ioctls in
`performCommand`, reports the Wi-Fi subfamily and the Infrastructure role, posts the apple80211 kernel events (power, scan done,
link, association done) with `kev_post_msg`, and passes EAPOL frames up the interface to eapolclient. This is the model of our own
earlier UniXNU-sheng `WifiNet.cpp` (BSD-2-Clause-Patent; its 656-byte association and inline-element scan layouts match what 27.3
builds). No Apple class is subclassed, nothing is added to the collection. airportd's launch on the controller match is the open
point: if it does not start by itself, a LaunchDaemon of ours starts it (`launchctl kickstart`), to be measured on M1.

M1 (one boot, the Ethernet path as control behind `piano-wifi-apple80211=1`): Wi-Fi: On in the menu, the scan list shown, power
off/on answered. M2: ASSOCIATE from the menu with the password, EAPOL to eapolclient, CIPHER_KEY into our key installation, the
port authorized, DHCP; our supplicant and profile file then stay as the fallback (`piano-wifi-apple80211=0`).

## 5. M1 as built (2026-11-14; `piano-wifi-apple80211=1`, default off)

Built and host-tested, **not run on the tablet**. Everything below marked "27.3" comes from the two host analyses of 2026-11-14
(the W0 report and a second pass over IO80211.framework, SystemConfiguration and airportd that answered the bind, the events
and the layouts): static facts, to be confirmed by the probe of §5.8.

### 5.1 What starts

`PianoWifi80211` (`package/PianoWifi/PianoWifi80211.h`, compiled into `PianoWifiEthernet.cpp`'s translation unit) is a subclass of
`PianoWifiEthernet`, so the data path, the link, the join from the profile, the scan list's copy and `piano-wifi-en0` are
`PianoWifiEthernet`'s, unchanged. Its class chain is `PianoWifi80211 < IO80211Controller < PianoWifiEthernet < IOEthernetController`:
`IO80211Controller` is a class of ours with nothing but the name, because both launchd's match for airportd (`IOProviderClass`)
and SystemConfiguration's Wi-Fi test (`IOObjectConformsTo`) compare class names up the OSMetaClass chain. IO80211Family is not in
our native collection, so the name is free; if the collection ever links Apple's family, our class must be renamed (the kext
would not load). No Apple class is subclassed and nothing is added to the collection.

Its personality (`tooling/k27/kext/PianoWifiEthernet.json`, `PianoWifi80211`) matches the same resource (`PianoWifiLink`) in the
same match category as `PianoWifiEthernet`'s, with probe score 1000; its `probe()` declines without the switch, and then
`PianoWifiEthernet` starts as before. With the switch exactly one of the two starts.

Order at its start: `PianoWifiEthernet::start` attaches the interface; inside it `configureInterface` gives the interface the Wi-Fi
subfamily and `IO80211InterfaceRole = Infrastructure` before it is registered; then a 200 ms poll waits for the BSD interface
(at most 180 s); once it exists the interface filter is attached, `IOInterfaceName` is made sure of, the power-changed and
link-down events are posted, the controller gets `piano-wifi-apple80211-answering = <BSD name>` and is **registered**. Only from
that moment can launchd's match start airportd, so airportd never meets an interface that does not answer.

### 5.2 What airportd, IO80211 and SystemConfiguration check (27.3), and what answers it

| Check (27.3) | Answered by |
|---|---|
| IO80211's interface list: `getifaddrs`, then `SIOCGIFTYPE` must give `ift_type` 6 (Ethernet) and `ift_subfamily` 3 (Wi-Fi); `ift_family` is not read (`SIOCGIFMEDIA` is used only on another path; its `EPWROFF` does not count) | the subfamily, §5.3 |
| The service of the name: `IOServiceGetMatchingService({IOPropertyMatch: {IOInterfaceName: name}})`, any class; on that same object (no parent search) `IO80211InterfaceRole` must be the string `Infrastructure`; without a service the bind fails (-3903) | the interface's properties (the family's `IOInterfaceName`, set by us if absent) |
| The bind's user client: `IOServiceOpen` on that object; any failure falls back to the ioctl path (logged by IO80211, not an error) | `IONetworkInterface` offers none: the ioctl path |
| The ioctl path's only sanity request: `SIOCGA80211` CARD_CAPABILITIES (12) with 23 bytes: 0 binds, anything else fails | a 23-byte raw bitmap, §5.4 |
| airportd's set-up: an `AF_LINK` address of type 6, the bind, DRIVER_VERSION (43) must answer (else "Unable to retrieve driver version"), the kernel-event monitor must open, then events enabled, CARD_CAPABILITIES, VIRTUAL_IF_ROLE (96: an answer would make the interface a virtual one), INFRA_ENUMERATED (512, set; its result ignored) | §5.4, §5.5 |
| SystemConfiguration (`networksetup`, Settings): `IOInterfaceType` 6 and the interface's parent conforms to `IO80211Controller` → "Wi-Fi", built in; the MAC from the controller's `IOMACAddress` | the class chain; `IOEthernetController`'s own properties |
| airportd's start: its job (`com.apple.airportd.plist`) has `LaunchEvents` `com.apple.iokit.matching` {`IOMatchLaunchStream`, `IOProviderClass = IO80211Controller`} and is also a MachServices job (`com.apple.airportd`, `com.apple.corewlan-xpc`, `com.apple.wifi-xpc`, ...), so a CoreWLAN client starts it on demand | the controller's registration; §5.6 |

### 5.3 The Wi-Fi subfamily

`IOEthernetInterface`'s public way to shape the ifnet is `initIfnetParams(ifnet_init_params *)`, whose (legacy) structure has no
subfamily. The 27.3 IONetworkingFamily has `IONetworkInterface::setInterfaceSubType(unsigned int)`: a non-virtual member, an
exported symbol of the family, **absent from the SDK/KDK header**. Its 27.3 code stores the value in the interface's reserved
block while the interface is not attached (and returns true then), and `IONetworkInterface::attachToDataLinkLayer` copies that
word into `ifnet_init_eparams.subfamily` (offset 0x140, as `kpi_interface.h` gives it) when the network stack attaches the
interface. We call it by its mangled name (`__ZN18IONetworkInterface19setInterfaceSubTypeEj`, an `asm` label: the object in x0,
the value in w1) from `configureInterface`, before the interface is registered. This is the one non-header symbol M1 uses; the
KC link resolves it from IONetworkingFamily's exports like any other family symbol (`check-kext-imports.py` lists it among the
library imports). If a future family drops it, the smallest alternative is an ifnet of our own (`ifnet_allocate_extended` with
`IFNET_SUBFAMILY_WIFI`, as sheng's non-macOS path does): `SIOCGIFTYPE` is answered by the BSD layer from the ifnet, so no filter
can change it. What the subfamily changes in xnu: Wi-Fi accounting and policy (`IFNET_IS_WIFI`) and the compatibility nexus's
ring sizes; on macOS the compatibility netif is attached as for any Ethernet interface.

### 5.4 The requests

An interface filter (`iflt_attach`, the BSD interface-filter KPI) sees every ioctl on the interface before the interface's own
handler; for `SIOCSA80211`/`SIOCGA80211` it answers from `package/PianoWifi/WifiApple80211.h` and returns `EJUSTRETURN` (answered:
`ifnet_ioctl` returns 0 and nothing after the filter runs) or the error; every other ioctl passes on (`EOPNOTSUPP`), its first
occurrence logged. The answers run in the requesting thread under the kext's lock, with a 2 KiB scratch buffer from the heap; the
stack check bounds the filter's own frames (928 bytes, limit 3 KiB).

| Request | Get / set | Answer (M1) | Layout (27.3) |
|---|---|---|---|
| DRIVER_VERSION 43, HARDWARE_VERSION 44 | get | "PianoWifi 1.0 (apple80211 M1)", "Qualcomm WCN7860" | 384 bytes, a NUL-terminated string from offset 0, no version word |
| STATUS_DEV_NAME 23 | get | "PianoWifi" | not asked by 27.3's bind; a string from 0 |
| CARD_CAPABILITIES 12 | get | bits AES, AES-CCM, power management, short slot, short preamble, WPA2, short GI 20 MHz, TSN; not 46 (wait for scan-done events) nor 73 (directed scans as SCAN_REQ_MULTIPLE) | a raw 23-byte bitmap from 0; `req_len` set to 23 |
| POWER 19 | get / set | {1, 1 radio, on/off}; a set records it and posts POWER_CHANGED; off makes SCAN_REQ and SCAN_RESULT answer `EPWROFF` (82). The chip stays up (M1) | 24 bytes; airportd's set first gets the radio count |
| STATE 13, OP_MODE 15 | get | INIT (0); station (1) | STATE {1, value} in 8 bytes; OP_MODE in `req_val` |
| PHY_MODE 14 | get | supported auto/a/b/g/n/ac, active none | 12 bytes |
| NOISE 17 | get | -95 dBm, one radio | 52 bytes |
| RSSI 16, CHANNEL 4, SSID 1, BSSID 9, CURRENT_NETWORK 103, RATE 8 | get | `ENOENT`: not associated (M2) | 52 / 16 / 32 (the length back in `req_len`) / 6 / 2280 bytes |
| COUNTRY_CODE 51 | get / set | "XX" (the list keeps no Country element); a set is accepted | 3 bytes, a C string |
| SUPPORTED_CHANNELS 27, HW_SUPPORTED_CHANNELS 254 | get | the 22 channels this station scans (2.4 GHz 1-13, 5 GHz 36-48 and 149-165), 20 MHz, active | 4824 bytes: the count at 4, entries {1, channel, flags} from 8 |
| SCAN_REQ 10, SCAN_REQ_MULTIPLE 86 | set | the scan list refreshed from PianoWifi's `piano-wifi-scan` (§5.5), the cursor to the first; the completion event 2 s later | 5528 / 5352 bytes; the SSID's length at 0x10 (logged as directed or not, never the name) |
| SCAN_RESULT 11 | get | one network per call, then `EIO` (5) and the cursor back to the first | the 2280-byte record; we write its head (sheng's 24A446 fields, unchanged on 27.3) and the elements, never its tail from 0x88c (user space puts a request identifier there) |
| SCANCACHE_CLEAR 90 | set | the snapshot emptied: SCAN_RESULT ends at once until the next scan | — |
| ASSOCIATE_RESULT 21 | get | 0 (nothing asked) | `req_val` |
| VIRTUAL_IF_ROLE 96, INFRA_ENUMERATED 512, CHANNELS_INFO 207, ASSOCIATE 20, CIPHER_KEY 3, any other | both | `EOPNOTSUPP` (102), logged once per kind | CHANNELS_INFO is 10016 bytes on 27.3; not needed for the bring-up |

A scan record's elements are rebuilt from what the list keeps: SSID, the supported and extended rates, the DS parameter set
(2.4 GHz), the RSN element as the frame carried it; the capability field is the frame's (ESS, privacy). For that the scan list
(PianoWifi.cpp `publishScan`) now also carries `ssid-raw`, `capability`, `beacon-interval`, `rates` and `rsn` for each network.

### 5.5 The scan list and the events

M1's scans are answered from **the list level 6 published at its scan**: the radio is not asked again, because the station may be
joined and a scan would take it off channel. The list is re-read at every SCAN_REQ, so whatever PianoWifi publishes later is
seen. A live scan on airportd's request is M1b (a boundary function into PianoWifi's ladder).

Events are kernel events of vendor Apple, class 6, subclass 1 (`kev_post_msg`, the private poster: the public `kev_msg_post`
refuses Apple's vendor code), the first data vector the 16-byte BSD name, the second the payload; 27.3's reader requires exactly
that (the name compared with the bound one, the payload from byte 0x28 of the message). Posted: POWER_CHANGED (1) at the
registration and at each POWER set, LINK_CHANGED (4) down at the registration (sheng's 32-byte payload), SCAN_DONE (10) 2 s after
each accepted scan (one per scan; an earlier one still waiting is posted at once). 27.3's airportd enables neither 9 nor 10: it
scans synchronously and reads the results right after SCAN_REQ returns, which answering from the list suits.

### 5.6 airportd's start: no daemon of ours should be needed

Inferred from the job file (§5.2): the registration fires launchd's `IOMatchLaunchStream` match, and a CoreWLAN client (the menu
extra, Settings) starts airportd on demand through its Mach services anyway. For the case that neither happens, the unit
`piano-airportd-kick` (`configs/userland.toml`; a script and a job, no Mach-O, so no trust-cache change) waits up to 300 s for
`piano-wifi-apple80211-answering`, gives launchd 10 s, and `launchctl kickstart`s airportd once if it is not running; without the
switch it does nothing. **It is not in the wm1 probe's seed** (the v2 seed predates it): the checklist does its one command by
hand when needed, and that measurement decides whether the unit stays.

### 5.7 Open after the build (what the probe answers, in order of risk)

1. **The end of a scan's results over ioctls.** 27.3's synchronous `Apple80211Scan` ends its result loop well only when the get
   returns exactly 5, and fails after 9999 records. Whether the ioctl path hands it `errno` (then our `EIO` ends the list well) or
   `ioctl()`'s -1 (then every scan over ioctls fails and drops its results) was not settled on the host. The probe shows it: the
   `SCAN_RESULT ... -> 5` line followed by networks in CoreWLAN and the menu, or a failed scan (`piano-cwscan scan` prints the
   error). If it fails, M1b is a user client on the interface for IO80211's bind (selector 0: an ioctl in a 0x3c30-byte structure;
   1: `useIOUCWhenPossible`; 7: DriverKit support; 5: no event pipe), through which the driver's errno comes back as the call's
   return value.
2. Whether the subfamily is taken (`subfamily 3 taken`, `subfamily 3` in the filter's line, `ifconfig -v`).
3. Whether launchd's match starts airportd by itself (§5.6).
4. The answers' 27.3 layouts not exercised by any host run: POWER and the scan record's head (sheng's 24A446 fields).
5. What the menu shows for a network whose elements are rebuilt (no HT/VHT elements: its PHY mode may read 802.11a/g).

### 5.8 Build, host tests and the probe

`tooling/k27/build-native-piano-wifi-ethernet.sh` runs `package/PianoWifi/tests/apple80211_host.cpp` (the layouts' sizes and
offsets; a model of user space asking what the bind and the menu ask: the identity, power off and on, the channels, a scan and
its records until the end with the record's tail kept, the cache clear, the not-associated answers, the unknown requests; the
address parse, the rebuilt elements, the log's table), compiles the kext, bounds the stack of the filter and the thread calls
(`check-kext-stack.py`, 3 KiB each) and checks the imports against the kernel (`check-kext-imports.py`: 0 unresolved; the family's
symbols, `setInterfaceSubType` among them, and PianoWifi's are the KC link's).

**Probe `ram-k273-wm1`** (§5.9: path, sha256, the build's numbers): the installed v4 entry's line (`out/persistent/esp-k273-v4.json`
`cmdlines["macos-27.3"]`) without `piano-boot-args=macos` and `data-store-lba`, with `data-persist=0` and the entry's tail
(`piano-wifi=8 piano-wifi-l8=55 piano-wifi-defer=3 piano-wifi-sleepoff=1`, the USB shell and network, ...), plus
`piano-wifi-apple80211=1 piano-reset-after=900 piano-reset-to=bootloader`; the v2 seed (`seed-200.img`) and the v4 layer's trust
cache. **Do not place the join profile until the M1 rows are read**: M1 is the menu and the scan, not a join; the station stays
idle and the scan list is level 6's.

Checklist (console: `tooling/native/usb-console.py`; commands over ssh or the USB root shell; `$I` is the BSD name of the second
row):

| When | Line / command | Expected |
|---|---|---|
| level 6's end | `PianoWifi80211: the interface is a Wi-Fi one: subfamily 3 taken, IO80211InterfaceRole Infrastructure`, `PianoWifi80211: started (piano-wifi-apple80211=1): N network(s) in PianoWifi's scan list; waiting for the BSD interface`, `PianoWifiEthernet: the interface is published (...)` | N ≥ 1; one controller only |
| the BSD name | `PianoWifi80211: $I: apple80211 requests answered from now on (after N ms); the BSD interface reads type 6, family 2, subfamily 3; IOInterfaceName the family's`, `$I: event 1 (0 payload bytes) posted -> 0`, `$I: event 4 (32 payload bytes) posted -> 0`, `$I: the controller is registered (...)` | subfamily 3; events `-> 0` |
| airportd's bind | `$I: get 12 CARD_CAPABILITIES len 23 val 0 -> 0 (wrote 23, len 23, val 0)`, `$I: get 43 DRIVER_VERSION len 384 val 0 -> 0 (wrote 384, ...)` | both `-> 0`; then one line for each further kind airportd asks (POWER, STATE, VIRTUAL_IF_ROLE `-> 102`, INFRA_ENUMERATED `-> 102`, ...) |
| +5 s, then each minute | `PianoWifi80211: $I (summary): R request(s) of K kind(s), U unknown; S scan(s), N result(s) served, E list end(s), ...; the controller's clients: ...` | R grows while airportd runs |
| ssh | `ifconfig -v $I`; `networksetup -listallhardwareports` | `Hardware Port: Wi-Fi`, `Device: $I` |
| ssh | `ioreg -l -r -c PianoWifi80211 -d 2 \| grep -E 'IO80211InterfaceRole\|IOInterfaceName\|piano-wifi-apple80211'`; `ioreg -c IO80211Controller \| head -3` | the role on the interface; the controller found by the class name |
| ssh | `launchctl print system/com.apple.airportd \| grep -E 'state =\|pid ='`; if not running, `sudo launchctl kickstart -p system/com.apple.airportd` (the unit's one command; record which case it was) | `state = running` |
| ssh | `sudo wdutil info \| sed -n '/WIFI/,/BLUETOOTH/p' \| grep -E 'Interface Name\|Power\|Op Mode\|MAC Address'` | the interface, power on |
| ssh | `log show --last 15m --predicate 'process == "airportd"' --style compact \| grep -iE 'BSD driver init\|driver version\|Falling back\|bind\|scan' \| head -40` | no "BSD driver init failed"; IO80211's "Falling back to IOCTL path" is expected |
| ssh, the scan | copy `out/piano-cwscan/piano-cwscan` (`tooling/native/piano-cwscan`, ad hoc signed; if AMFI refuses it, go on with the next row) and run `./piano-cwscan scan` | `interfaces 1: $I`, power on; `scan: M network(s) ...` with M about N, or `scan failed: domain ... code ...` (§5.7 item 1) |
| ssh, the scan | `system_profiler SPAirPortDataType > /tmp/ap.txt; grep -c 'Security:' /tmp/ap.txt` (counts only; never copy the names off the tablet) | about N |
| the console during the scans | `$I: set 10 SCAN_REQ len 5528 ... -> 0 (...); N network(s)`, `$I: get 11 SCAN_RESULT len 2280 ... -> 0 (...); cursor 1 of N`, `$I: get 11 SCAN_RESULT len 2280 ... -> 5 (...)` | the end once per scan |
| power off and on | `networksetup -setairportpower $I off`, then `on` (each goes through airportd) | `$I: set 19 POWER len 24 ... -> 0`, `$I: POWER set off by airportd ...`, then `on`; `networksetup -getairportpower $I` follows them |
| the screen | webcam `imagesnap -d "HD Pro Webcam C920" /tmp/wm1-menubar.jpg`; then click the menu bar's Wi-Fi icon (`piano-input.py`) and photograph the open menu | the Wi-Fi icon in the menu bar; the open menu lists networks (count them; record no names) |
| last: the data path's control | place the profile (`/tmp/wifi-profile-place.sh`, as for 10i): the station joins as on 10i; `ipconfig getifaddr $I`, `ping -c 5 <router>` | as 10i: an address and 5/5, now on an interface typed Wi-Fi (airportd still says not associated: M2) |
| the end | console `reset fastboot` once the rows are read | — |
