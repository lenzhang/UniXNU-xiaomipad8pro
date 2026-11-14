# The macOS Wi-Fi menu: PianoWifi under Apple's Wi-Fi stack (design and first probes, 2026-11-14)

Status: the narrow apple80211 binding is built; M1 is partial. The Ethernet link works since boot 10i (WPA2, DHCP and Internet
over `PianoWifiEthernet` with our own supplicant). 10m added park/restart at sleep and restart after a firmware crash. Installed
ESP v4 auto-joined after a plain reboot; **v5, installed 2026-11-14 19:00 with the 10m kernel, auto-joined on its first boot**
([wifi.md](wifi.md) §8.8, §13). On the Apple path, wm1 made en2 a Wi-Fi hardware port and served scans to airportd (17 networks,
the list's end answered), but airportd's corewifi side did not adopt it (`wdutil`: Interface Name (null)); wm1b then made
CoreWLAN enumerate it (§6.1b). M1b's accept modes are a run-time switch (§6.2) and probe `ram-k273-wm2` (§6.3, §6.4) is built for
them. There is no menu network list or menu-driven join yet. The owner's next step remains the Wi-Fi menu, Settings › Wi-Fi and
CoreWLAN, through airportd and IO80211Family, as on a Mac.

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
| W4 | Known networks and auto-join at boot (airportd's), roaming between the same SSID's access points (our rejoin), sleep/wake (10m's park-and-restart) | after a reboot the menu joins by itself; Settings › Wi-Fi works |

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
and eapolclient); the first tablet checks are in §5:

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
builds). No Apple class is subclassed, nothing is added to the collection. **airportd did start by itself on wm1**; a helper
LaunchDaemon is not needed for the launch. airportd's CoreWLAN cache and a complete CoreWLAN scan result remain the open M1
points (§5).

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

### 5.9 The probe as built

`out/ram-k273-wm1` (in the M1 branch's worktree; built 2026-11-14 18:50 from branch commit `82d8b53`): 237281280 bytes, sha256
`452d0db9f23eae9e9cdf222e25750a9a1f1a8c45248bf39b64d464838a43a6bb`, `verify-ram-images.py` PASS. The 27.3 collection
`01d21b4d...` (`tooling/k273/build-k273-image.sh`, the 0066 27.3 kernel `8b70f531...`, 26 native kexts all this repository's,
`vtable check PianoWifiEthernet vs IOEthernetController: 0 problem slots`, the KC link resolving `setInterfaceSubType` from
IONetworkingFamily); the v2 seed (identity `9bd6f902...`, `data-persist=0`: the owner's store is not opened), the v4 layer's
trust cache `57063d6d...`; 27 arguments (492 bytes) in `/chosen/piano-boot-args-ram`, the merged line 1160 bytes, with
`piano-wifi-apple80211=1 piano-reset-after=900 piano-reset-to=bootloader`. Its dump image: `out/ram-k273-wm1-dump` (sha256
`65ac4f35165d32b1245ab837823f9dc73750501084fd0f7d0a4e4f9492f02f1d`, the same collection). `piano-cwscan` is beside the image.
The image's own control is boot 10i/10l (the same line without the switch): their `PianoWifiEthernet:` lines are what
`PianoWifi80211`'s parent must still print.

## 6. M1 on the tablet, and M1b

### 6.1 wm1 (2026-11-14 19:01-19:18; private/host-checks/20261114-wm1)

**Confirmed on the tablet:**
- SystemConfiguration: `networksetup -listallhardwareports` lists `Hardware Port: Wi-Fi`, `Device: en2`; `ifconfig -v en2` says
  `type: Wi-Fi` and carries the agent domain `WiFiManager ... desc:"WiFi"`; `ioreg` shows the role and
  `piano-wifi-apple80211-answering = en2`. The subfamily was taken (`type 6, family 2, subfamily 3` in the filter's line).
- airportd started by itself (`state = running`, pid 132), through launchd's IOKit match on the class name: the starter unit is
  not needed (§5.6 inferred it; now measured).
- The bind and set-up: CARD_CAPABILITIES (23 bytes), DRIVER_VERSION (384), COUNTRY_CODE, PHY_MODE, OP_MODE, NOISE, POWER get and
  set, POWERSAVE set (value 7), SUPPORTED_CHANNELS (272 bytes written): all answered `-> 0`. The events 1, 4 and 10 were posted
  `-> 0`.
- Scans over ioctls work: `SCAN_REQ len 5528 -> 0` with 17 networks, `SCAN_RESULT` one per call (`cursor 1 of 17` ...) and the end
  `-> 5`, repeated (57 scans, 969 results served in the run's summaries): the end-of-list question of §5.7 item 1 is answered,
  airportd does not fail the scan.
- SSID, BSSID, CHANNEL, RSSI, CURRENT_NETWORK `-> 2` (not associated), as designed.

**Not working:** `wdutil info` reports `Interface Name: (null)`, `Power: Off`, `Op Mode: None`, `Supported Channels: (null)`;
airportd's log keeps scheduling scans and says `AUTO-JOIN: -- no_alt_interface`, and corewifi logs
`-[CWFXPCRequestProxy __apple80211WithVirtualInterfaceRole:parentInterfaceName:] ... Failed to match ...` (the line was cut by
the capture). So airportd's corewifi side has not adopted en2 as its Wi-Fi interface although every request it sends is answered;
CoreWLAN's `scanForNetworks` did not return within minutes (it held the session's ssh: `piano-cwscan` now ends itself).

The requests airportd asked that we refused (`-> 102`), by kind: get 207 CHANNELS_INFO (10016 bytes), get 96 VIRTUAL_IF_ROLE,
set 512 INFRA_ENUMERATED (1 byte), set 85 (2068), set 490 (300), set 469 (8), set 575 (4), get 108 (88), get 115 (2600), get 97
(16), get 87 (8), get 174 (24), get 175 (16), get 194 (8), get 353 / 393 / 55 / 57 (no buffer).

### 6.1b wm1b (2026-11-14 19:46-19:57; private/host-checks/20261114-wm1)

**wm1b, 19:46-19:57** (`wm1b-remote.txt`, `wm1b-console.out`): the CoreWLAN helper enumerated en2 with power on, mode 1, an
active service and 22 channels; power off/on worked, and System Profiler listed a third-party wireless card. CoreWLAN's scan
returned zero networks in 15 ms (wm1's scan never returned; the accept modes of §6.2 were in between). No menu network list or
join has passed yet.

### 6.2 M1b: what the next boot varies

The adoption is decided in airportd/corewifi; which of its inputs is missing cannot be read off our log. M1b therefore makes the
answers to the refused requests a **run-time switch**, so that one boot tries each and the tablet shows which one changes
`wdutil`'s Interface Name. No layout is guessed beyond "accepted" and "zeros":

| `machdep.piano_wifi_a80211` bit | Effect |
|---|---|
| 0x1 (the default) | INFRA_ENUMERATED (512) set accepted |
| 0x2 | every other set without a handler accepted (0), its value ignored; never the join's sets (ASSOCIATE, DISASSOCIATE, DEAUTH, CIPHER_KEY, AUTH_TYPE, RSN_IE: M2) |
| 0x4 | every other get without a handler answered with zeros of the asked length (0 in `req_val` without a buffer); not VIRTUAL_IF_ROLE |
| 0x8 | VIRTUAL_IF_ROLE (96) get answered 0 in `req_val` (else refused: an answer may make airportd take the interface for a virtual one) |

`piano-wifi-apple80211-accept=N` sets the mode at boot; `sudo sysctl machdep.piano_wifi_a80211=N` changes it at run time and logs
`PianoWifi80211: en2: accept mode 0xN (was 0xM, machdep.piano_wifi_a80211): every request kind is logged again from here`; the
one-line-per-kind log starts over, so the requests airportd asks after the change (and their answers, `[accept mode]` marked)
are all in the log again. airportd sets the interface up once, at its start: after a change, `launchctl kickstart -k
system/com.apple.airportd` makes it start over with the new answers. The summary line carries the mode and how many requests it
answered.

### 6.3 Probe wm2 and its checklist

`out/ram-k273-wm2`, the installed v5 entry's recipe (`out/persistent/esp-k273-v5.json`'s line and tail, `data-persist=0`, the v2
seed `seed-200.img`, the v4 layer's trust cache) and `piano-wifi-apple80211=1 piano-reset-after=900 piano-reset-to=bootloader`;
its build is in §6.4. No join profile in this boot.

Every tablet command has a time limit. Over ssh, the Mac's side limits the session; on the tablet, `bash -c` with this helper
(the commands below use it):

    tmo() { s=$1; shift; "$@" & p=$!; i=0; while kill -0 $p 2>/dev/null && [ $i -lt $s ]; do sleep 1; i=$((i+1)); done; kill -9 $p 2>/dev/null; wait $p 2>/dev/null; }

`piano-cwscan scan 45` ends itself after 45 s. `wdutil` is `sudo wdutil info | sed -n '/^WIFI/,/^BLUETOOTH/p' | grep -E 'Interface
Name|Power|Op Mode|Supported Channels'` (W below). The adoption log (A below):
`tmo 30 log show --last 3m --style compact --predicate 'process == "airportd"' | grep -iE 'Failed to match|no_alt_interface|interfaceName|virtualInterfaceRole|primary|adopt|attach|BSD driver' | head -40`
(keep the lines whole; record the interface names only, never a network's).

| Step | Command / line | Read |
|---|---|---|
| boot | the M1 lines of §5.8 again, then `PianoWifi80211: accept mode 0x1 (machdep.piano_wifi_a80211 changes it at run time: ...)` and `en2: set 512 INFRA_ENUMERATED len 1 ... -> 0 (...) [accept mode]` | the default mode in effect |
| mode 0x1, 2 min after the controller's registration | W; A; `sysctl machdep.piano_wifi_a80211` | the control: Interface Name (null) as on wm1, or not |
| mode 0x3 | `sudo sysctl machdep.piano_wifi_a80211=3`; `sudo launchctl kickstart -k system/com.apple.airportd`; wait 60 s; W; A | the new kinds' lines (`[accept mode]`), Interface Name |
| mode 0x7 | `=7`, kickstart -k, 60 s, W, A | the same |
| mode 0xf | `=15`, kickstart -k, 60 s, W, A | the same; VIRTUAL_IF_ROLE `-> 0 [accept mode]` |
| at the first mode where Interface Name is `en2` | `tmo 50 ./piano-cwscan scan 45`; `tmo 40 system_profiler SPAirPortDataType > /tmp/ap.txt; grep -c 'Security:' /tmp/ap.txt`; webcam the menu bar, open the Wi-Fi menu (`piano-input.py`), photograph it | CoreWLAN's count and the menu's list (counts only) |
| if no mode changes it | the summary lines (`accept mode 0x.. (N answered by it)`), the full lines of A for each mode | what airportd still lacks: the next step is the IOUserClient bind (§5.7 item 1's selectors), decided on this evidence |
| the end | console `reset fastboot` | — |

### 6.4 wm2 as built

`out/ram-k273-wm2` (the M1 branch's worktree; built 2026-11-14 from branch commit `765f0b9`, main `bc5e5da` merged): 237281280
bytes, sha256 `e5fe6e503c3ea482b34093e87dd0299b206b34cdd3321986cbc1e36c20681d1f`, `verify-ram-images.py` PASS. The 27.3 collection
`a598579c...` (the 0066 kernel, 26 native kexts all this repository's, `vtable check PianoWifiEthernet vs IOEthernetController:
0 problem slots`); the v2 seed (identity `9bd6f902...`, `data-persist=0`), the v4 layer's trust cache `57063d6d...`; the
arguments' tail 496 bytes in `/chosen/piano-boot-args-ram`, the merged line 1164 bytes. Dump image `out/ram-k273-wm2-dump`
(sha256 `c41f00f504a8aa193a81ef982f06a3d674a6ed87f9631df46a9c391477e3ebd1`); `piano-cwscan` beside the image.
### 6.5 What wm2 settled, and what it did not

The accept-mode sweep (1, 3, 7, 15) changed nothing: `wdutil` kept `Interface Name: (null)`, corewifi's lookups kept missing, and
no answer of ours gates adoption. The diagnosis is `private/host-checks/20261114-flash/wm2-diag.md` (§5 ranks the causes, §6 lists
the log gaps). Its ranking, all **inferred**: (1) no user client to bind — over a whole boot the only client of our service was
the family's own `IOEthernetInterface`; (2) WiFiManager may want the *interface* to be of a class named `IO80211Interface`;
(3) our events carry no payload; (4) corewifi asks for an awdl companion with no parent. §7 (wm3) tries 1 to 3 on one boot.


## 7. M1b (wm3): the controller's identity for CoreWiFi, and the user client

Built, **not run on the tablet**. One boot, three stages switched at run time; stage 0 is wm2, so the boot carries its own control.

### 7.1 Why the identity, and why it is first

Two host analyses now disagree about what adoption needs, and wm3 settles both on one boot:

- `private/host-checks/20261114-io80211/ADOPTION.md` (strings, imports and plists; no disassembly): 27.3's **CoreWiFi has its own
  discovery** (`__findWiFiController`, the `CWFIO80211` cluster) keyed on registry properties — `IOClass`, `BSD Name`,
  `IOInterfaceName`/`Prefix`/`Unit`, `IOEthernetInterface`, and **`IOClassNameOverride` / `IO80211Controller`**. It imports no
  `IOObjectConformsTo`, so the class-name chain that satisfies launchd and SystemConfiguration says nothing to it; and it imports
  no `IOServiceOpen`/`IOConnectCall*`, so a user client is demoted. **P0: the one discovery input we lack is
  `IOClassNameOverride = "IO80211Controller"` on the controller.**
- `private/host-checks/20261114-flash/wm2-diag.md` §5 Rank 1: the only client of our service over a whole boot was the family's
  own interface, so a user client to bind might be what is missing.

Both are inferred. The property is nearly free and is tried first; the user client rides along on the same boot.

### 7.2 The switches

`piano-wifi-apple80211-uc=N` at boot, `sudo sysctl machdep.piano_wifi_a80211_uc=N` at run time (0 = wm2's behaviour):

| Bit | What |
|---|---|
| 0x8 | `IOClassNameOverride = "IO80211Controller"` on the controller (ADOPTION.md P0). At boot it is set **before the interface registers** (the production form of P0: CoreWiFi cannot meet the child before the controller's identity is there); by sysctl it is set late, which is the A/B of the same thing and needs airportd restarted after it |
| 0x20 | the same override on the interface object too |
| 0x1 | `newUserClient` answers: a client of ours (`PianoWifi80211Uc.h`) is made. Without it the call is refused and passed to the family — **but every call is logged either way**, so even stage 0 says whether anything tries to open us |
| 0x2 | the controller also carries `IOInterfaceName`, the role and the name prefix, so a lookup by `IOInterfaceName` (which takes any class) can find and open the controller |
| 0x4 | the events carry payloads (POWER_CHANGED the power structure, SCAN_DONE a status word) instead of none (wm2-diag Rank 3) |
| 0x10 | selector 5 answers 1 (the family's event pipe) instead of 0 (keep the kernel-event socket) |

Not in wm3: a country code other than XX, an awdl stub, another accept mode (ADOPTION.md §5 P1 and "demoted"). The accept mode
stays `0x1` for every stage, so the stages have one variable each.

Rank 2 of wm2-diag (an interface object of a class named `IO80211Interface`) **cannot be built**: the 27.3 IONetworkingFamily does
not export `IONetworkInterface`'s reserved slot 10, so a subclass of `IOEthernetInterface` does not link. The override property
is what replaces it, and it is what CoreWiFi reads anyway.

### 7.3 The user client

Selectors as the host analysis of 27.3's user-space IO80211 names them (§5.7 item 1): 0 and 6 are an apple80211 request in a
0x3c30- or 0x3c030-byte structure (the ioctl number, length and user pointer at its head; the name, request number and value at
0x3c08, 0x3c18 and 0x3c1c of the second part), answered by the same handler the ioctls use — **so the driver's own errno reaches
user space**, which the ioctl path cannot do; 1 `useIOUCWhenPossible` answers 1; 7 DriverKit answers 0; 5 the event pipe answers 0
unless 0x10. The pipe's own selectors (2, 3, 4) and the buffer mapping (8) are logged and refused, which is itself a result.

The five field offsets and the selector numbers are data from the host analysis; the answers are our own (`WifiApple80211.h`). No
Apple code or binary is in the repository.

### 7.4 wm3's checklist

Copy `tooling/native/tmo.sh` to the tablet first and prefix **every** command with it (`tmo 25 ...`); `piano-cwscan` also ends
itself. Three shorthands, used per stage:

- **I** (the identity, no network data):
  `tmo 20 ioreg -l -r -c PianoWifi80211 -d 2 | grep -E 'IOClassNameOverride|IOClass"|IO80211InterfaceRole|IOInterfaceName|IOInterfaceUnit|BSD Name|IOUserClient'`
- **W** (four fields): `tmo 25 sudo wdutil info | sed -n '/^WIFI/,/^BLUETOOTH/p' | grep -E 'Interface Name|Power|Op Mode|Supported Channels'`
- **A** (whole lines, no `head`, no `cut`; ADOPTION.md §6.2's pattern):

      tmo 40 log show --last 3m --info --debug --style compact --predicate 'process == "airportd"' > /tmp/ap.log
      grep -E '\[iflist\]|CWFIO80211|CWFApple80211|Failed to match cached|not a valid 80211|WiFi network interface added|Falling back to IOCTL|Invalid interface specified|Failed to get Apple80211|Failed to get country code|Failed to get supported channels|No channels configured|Configured scan manager|Starting scan sequence|Completed UI scan sequence|UI scan sequence finished' /tmp/ap.log

A scan step is: read the serial (`grep -c 'scan .* asked by' <console>` or the summary's `serial N`), run
`tmo 50 ./piano-cwscan scan 45`, read the serial again. If no serial appeared between them, CoreWLAN's request never reached the
driver; if one did, its completion line says how many records it took.

| Stage | Commands | Read |
|---|---|---|
| boot, stage 0 (= wm2) | the M1 lines; `PianoWifi80211: stage mode 0 (...)`; `identity (at the filter's attach): controller ... IOClassNameOverride (absent); interface class IOEthernetInterface, ... IOInterfaceName en2, ... IOInterfaceUnit 2; BSD en2`; the summary's first line (clients, user clients) | the identity line is complete; `IOInterfaceUnit`/`BSD Name` are in it (they were never printed before) |
| stage 0 | I, W, A, a scan step | the control: Interface Name (null), no `CWFApple80211`; a CoreWLAN scan that does not reach us (expected: the serial does not move) |
| stage 1 (P0) | `sudo sysctl machdep.piano_wifi_a80211_uc=8`; `sudo launchctl kickstart -k system/com.apple.airportd`; 60 s; I, W, A, a scan step | `identity (machdep...): ... IOClassNameOverride IO80211Controller ...`; **the question of the boot**: W's Interface Name `en2` and a `CWFApple80211`/`WiFi network interface added` line, or the same misses as stage 0 |
| stage 1b | `=40` (0x8 + 0x20: the interface too), kickstart -k, 60 s, I, W, A | whether the child's own override is what CoreWiFi wants |
| stage 2 | `=41` (0x8 + 0x20 + the user client), kickstart -k, 60 s, I, W, A | `a user client was opened: type T, task airportd[N]` and `selector S: struct in ... -> 0x0`, or no open at all (then nothing binds a client: that rank is refuted) |
| stage 2b, only if W shows `en2` at some stage | at that stage: a scan step, then `tmo 40 system_profiler SPAirPortDataType > /tmp/sp.txt; grep -c 'Security:' /tmp/sp.txt`; log in, then the webcam at the desktop and at the opened Wi-Fi menu (`piano-input.py`) | about 15 networks in both and the menu's list |
| if no stage adopts | keep I, W and A per stage, the scan serials, and A's `role=`/`parent=` lines; the next step is decided on that evidence (ADOPTION.md §5 P1's country code, or the event payloads `=4`) | — |
| the end | console `reset fastboot` | — |

### 7.5 wm3 as built

`out/ram-k273-wm3` (the M1 branch's worktree; built 2026-11-14 from branch commit `1ea2fa2`, main `d0d8aac` merged): 237281280
bytes, sha256 `5a9ac99c586a4a9ce0006c3678f69047c47a562d1e66819980b6b0c2b24e1364`, `verify-ram-images.py` PASS. The 27.3 collection
`9c02815c...` (the 0066 kernel, 26 native kexts all this repository's, `vtable check PianoWifiEthernet vs IOEthernetController:
0 problem slots`); the v5 recipe (its line and tail), the v2 seed (identity `9bd6f902...`, `data-persist=0`), the v4 layer's
trust cache `57063d6d...`; the arguments' tail 496 bytes, the merged line 1164 bytes, `piano-wifi-apple80211=1` and no
`-uc` argument, so it boots in stage 0 (= wm2) and every stage is a sysctl away. Dump image `out/ram-k273-wm3-dump` (sha256
`976c5c408bb1a1ffcb69b0170f5af7a11ab833b85b9dcfae634422930f78da9a`). `piano-cwscan` and `tmo.sh` are beside the image.

### 7.6 wm3 on the tablet (21:20) and wm3b

**wm3 answered nothing:** about 50 s into user space launchd rebooted "due to critical process crashes: configd", after thousands
of `decmpfs_pagein_compressed: err 12` / `apfs_pagein ... error 12` lines (ENOMEM page-ins; 27378 of them;
private/host-checks/20261114-wm3). **Inferred cause: the kernel's private data range was full, not PianoWifi80211.** The boot's own
lines give the budget: kmem's private data range is 2643 MiB (boot lines 22, 24), and NativeRamBlock takes 2048 MiB of it at once
for the volatile Data disk (`data-volatile-reserve-mib`, default the budget), leaving about 595 MiB for every other data allocation
in the kernel. The first refusal is PianoA830's (`gpu open refused: loginwindow ... NoMemory`, line 4313, with 519 MiB of its pool
in use), six lines before decmpfs's first failed decompression buffer (4319). PianoWifi80211 was in stage 0 there: three fixed
allocations under 16 KiB taken at its start and none per request (`apple80211_host` now bounds it: 35000 requests of every kind
change nothing), no user client asked for (0 opened, 0 refused), no scan yet.

**wm3b** carries `data-volatile-reserve-mib=1024` (the budget stays 2048 MiB: the pool grows past the reserve as Data is written;
wm2's Data peaked at 1189 MiB resident), so at the login burst about 1.6 GiB of the range stays free. This is a mitigation for the
probe; the budget itself (the reserve against the range, now that the GPU and the desktop share it) belongs to NativeRamBlock's
owner. The boot's `NativeRamBlock: volatile: ... reserved at start` line says which reserve it ran with; if wm3b fails the same way
with it, the cause is elsewhere.

`out/ram-k273-wm3b` (built from branch commit `1e25edf`, main merged; the 0066 kernel): 237281280 bytes, sha256
`5195d1553c3d16106f69d9803a4fedb5e747eb02be18b4ecb25a6e3e0d5c5337`, `verify-ram-images.py` PASS; collection `8469f62d...` (26 native
kexts all this repository's, the vtable check clean); the v5 recipe, the v2 seed volatile, the v4 trust cache; the merged line 1195
bytes. Dump image `out/ram-k273-wm3b-dump` (sha256 `17038b843c7d792439914562ba0c502461f2fc9bd6047565a50d6b7e1f95cc99`). The
checklist is §7.4's, unchanged, with one row first: `NativeRamBlock: volatile: ... 1024 MiB reserved at start`.

### 7.7 wm3b on the tablet: P0 confirmed, and the default since

**Confirmed on the tablet (wm3b, 2026-11-14 22:08-22:17, private/host-checks/20261114-wm3b):** with `IOClassNameOverride =
"IO80211Controller"` on the controller (stage 1), the airportd started by the kickstart logged `[iflist] Successfully created
CWFApple80211 (en2)`, `WiFi network interface added (en2), posted notification`, and created its SystemConfiguration service,
interface and 802.1X objects for en2; the stage-0 airportd (no override) had `Did NOT find matching WiFi interface ...
role=undefined/infra`. The data-range mitigation held: no page-in failure on this boot (1024 MiB reserved at start).

**Still not working at stages 1, 1b and 2:** `wdutil` keeps `Interface Name (null)`, `Power Off []`, and CoreWLAN's scan returns
0 networks in about 35 ms although each CoreWLAN scan reaches the driver (the scan serial moves by 3 per scan step, each scan
served its 19 records and its list end). Open; not settled here.

**Since then the override is the default** (`piano-wifi-apple80211-uc` defaults to 0x8), in P0's production form: set in
`configureInterface`, before the interface registers, whenever `piano-wifi-apple80211=1`. `piano-wifi-apple80211-uc=0` is the
control without it.

## 8. M2 plan: the menu's join (2026-11-14; condensed from the host study `private/host-checks/20261114-io80211/JOIN.md`)

M2 is §2's W3/W4 made concrete: joining when the user picks a network in the menu. The study's direct 27.3 facts
(REPORT.md §2-§4): the central join request is **SET 20 ASSOCIATE**, version 1, **656 bytes** (Apple's `Apple80211Associate2`
builds exactly this size; whether the current menu route through CoreWiFi's direct `Apple80211Set` uses the same wire buffer is
**inferred**); WPA2 runs in **eapolclient** in user space over an `AF_NDRV` raw socket on enX (EtherType 0x888e) and installs
the keys with **SET 3 CIPHER_KEY**, **148 bytes** (the size is confirmed from its call sites); the 27.3 event table has
SSID_CHANGED 2, BSSID_CHANGED 3, LINK_CHANGED 4, ASSOC_DONE 9, RSN_HANDSHAKE_DONE 20. Everything marked **inferred** below is a
hypothesis that the first M2 boot's request/event capture must confirm. In the implementation, **log every join-related request
and posted event** (serial, process, generation, number, length, result) and log credential/key **metadata only** — never key
bytes, passphrases or nonces.

### 8.1 What a menu join sends

The chain is symbol-confirmed: the menu's pick goes through CoreWLAN/CoreWiFi's XPC to airportd, which owns network selection,
passwords and policy, and issues apple80211 requests on enX; the driver associates, and for Apple-managed WPA2 airportd controls
eapolclient. Which route one menu action takes, and every fixed daemon order beyond the facts above, is **inferred** — the driver
must log what actually arrives and tolerate harmless reordering. The sequence to implement and instrument:

| Phase | Request or event | What our driver must do | Confidence |
|---|---|---|---|
| Before the join | GET 19 POWER, GET 13 STATE, the M1 gets | no driver action until airportd asks | request surface confirmed; the exact per-join set inferred |
| Refresh candidates | SET 10 SCAN_REQ (or 86), then GET 11 to the end | a directed scan may precede association; fresh cached results are acceptable | order inferred (normal airportd behaviour) |
| Old-link teardown | SET 22 DISASSOCIATE (no payload), sometimes SET 29 DEAUTH | stop the old generation, clear keys and the visible snapshot, keep the port closed | request classes confirmed; whether it precedes every join inferred |
| Security compatibility | SET 2 AUTH_TYPE, SET 46 RSN_IE | accept and record them per generation; **never require them as a separate preamble** — the association buffer already carries security and authentication parameters | requests confirmed; separate-call order inferred |
| Join | **SET 20 ASSOCIATE** (§8.2) | validate, clear stale visible state, allocate a generation, start the join asynchronously, return success only for acceptance | request and length confirmed |
| Progress | GET 13 STATE (0 INIT, 1 SCAN, 2 AUTH, 3 ASSOC, 4 RUN), GET 21 ASSOCIATE_RESULT (0 pending, 1 success, 2 failure, 9 timeout, 21 bad key; the numbers are prior art) | keep the 802.11 association and the port authorization distinct: ASSOC with a pending result while the WPA handshake runs, RUN only once authorized | state values prior art; polling order inferred |
| 802.11 association ends | **event 9 ASSOC_DONE** (§8.3) | where Apple's supplicant runs WPA, post after the 802.11 result while STATE is still ASSOC so the handshake can start; where ours runs WPA, hold the public completion until it authorizes | event code confirmed; payload and timing inferred |
| WPA2 handshake | EAPOL through enX, then **SET 3 CIPHER_KEY** (§8.2) | route EAPOL before authorization, translate and install the keys, keep ordinary data blocked | mechanism confirmed on 27.3; the exact call interleaving inferred |
| Security complete | **event 20 RSN_HANDSHAKE_DONE** | first try: once, after both keys and port authorization | code confirmed; payload and cardinality inferred |
| Connected | **2 SSID_CHANGED, 3 BSSID_CHANGED, 4 LINK_CHANGED up**, STATE=RUN | publish the snapshot before posting; requests 1/9/4/16/103 agree with it; then allow DHCP and data | consumers confirmed; the final order inferred |
| Leave/loss | SET 22 DISASSOCIATE, or an unsolicited link loss | clear keys and the current network, LINK_CHANGED down, complete any pending join exactly once | request/event classes confirmed |

For **open** networks skip the handshake rows: associate, authorize the peer at once, complete the result, publish RUN and the
connected events — no EAPOL, no CIPHER_KEY, no RSN event. For **PSK with our supplicant** (§8.4, mode A) EAPOL and keys stay on
our private control port and the public success waits for our AUTHORIZE.

Requests that must not gate the first milestone: GET 50 ASSOCIATION_STATUS (27.3 names it, its layout was not recovered — log it
if seen rather than guessing a response); ASSOCIATE_EXTENDED_RESULT (named, number unknown — do not invent one; request 21 stays
the result path); GET 156 LINK_CHANGED_EVENT_DATA (a prior-art reconstruction, a companion to our inline payload only); and
RSN_CONF 77, PMK_CACHE 83, CUR_PMK 376, RSN_XE 457, OFFLOAD_RSN 177 — broader caching/WPA3/offload paths where a clear
`EOPNOTSUPP` beats a fake success; plain WPA2-PSK through eapolclient needs no RSN offload.

### 8.2 The layouts to accept

**ASSOCIATE, 656 bytes**: version 1 confirmed on 27.3. The prefix is the layout our own sheng binding parses on iPadOS 24A446
(our BSD-2-Clause-Patent prior art): version at `0x00`; the authentication words widened to 32 bits at `0x08` (lower selector:
open system is 1) and `0x0c` (upper bitmask: WPA2-Personal is `0x10`); SSID length at `0x10` (1–32) with 32 raw SSID bytes at
`0x14`; a preferred BSSID at `0x34` (all zero means unspecified); the credential block from `0x3c`. The credential rules that
binding demonstrated: length zero is an open network; cipher **10** with 8–63 bytes is a passphrase; cipher **6** with exactly
32 bytes is a PMK; anything else is rejected rather than reinterpreted as open or as a password. Everything after the credential
window through `0x28f` is **unknown**: keep the whole request and log an unexpected length instead of truncating it into a guess.
The first WPA2-PSK/CCMP join builds its own RSN element from the selected scan entry's policy (our existing association-request
builder) instead of depending on the unrecovered trailing IE offset; a separate RSN_IE set, if received, is retained and checked
for consistency.

The other sizes: **AUTH_TYPE** 16 bytes (version, lower, upper, reserved, all 32-bit words; a 12-byte legacy set form is accepted
as a compatibility convenience; lower 1 = open system, upper `0x10` = WPA2-Personal in our working binding). **RSN_IE** 264 bytes
(prior art, not independently proven on 27.3): a version word, a 2-byte IE length at `4`, 257 bytes of IE storage at `6`, and the
IE is the **whole element** including its type and length bytes — validate `2 + ie[1]` against what was supplied. **CIPHER_KEY**
148 bytes, size confirmed on 27.3; its field map is Sonoma-era reconstruction, so the offsets are **inferred**: flags exactly
**4** is the pairwise key (install with index 0 and RSC 0), flags exactly **0** is the group key (use its key index and the low
six RSC bytes); Apple cipher **5** is AES-CCMP — translate it to our RSN suite type 4, never pass it through as an RSN suite
number; cipher 3 (TKIP) stays outside the first CCMP-only gate rather than being silently mapped; ciphers 6/10 inside ASSOCIATE
are credential forms, not key algorithms. PTK before GTK is the normal handshake order but our existing key sequencer enforces
it, not request timing; a group key that arrives first waits for pairwise completion. The peer address is a consistency check.
Ignore the WoW fields for now; wipe the whole request and every temporary key copy after use.

### 8.3 Events and their first payload candidates

- **ASSOC_DONE (9)** — code confirmed. First candidate: the 540-byte form our sheng binding uses for a 656-byte association
  (ten 52-byte attempt records from `0x000`, reason at `0x210`, result at `0x214`, result-valid at `0x218`): zero it, put the
  attempted BSSID in the first attempt record, set the result with valid=1, fill the reason when known. **Inferred from 24A446**,
  not a recovered 27.3 layout; keep request 21 correct even if the daemon ignores the event's payload.
- **LINK_CHANGED (4)** — our 32-byte inline candidate (down byte at `0`, RSSI at `4`, SNR at `8`, noise at `0xa`, reason at
  `0x14`) is the size M1 already posts; its 27.3 semantics are **inferred** — leave reason and the disconnect flags zero unless
  known. GET 156 is implemented only as a companion: the two prior-art models disagree about the disconnect flag's meaning, so
  neither is proof.
- **RSN_HANDSHAKE_DONE (20)** — code confirmed; an empty payload is the first candidate; post once after the group key plus peer
  authorization and instrument whether airportd/CoreWLAN act on it (the historical driver sometimes emitted one per key and
  sometimes none, which cannot settle our cardinality).
- **SSID_CHANGED (2) / BSSID_CHANGED (3)** — empty payload; the values are read through requests 1 and 9.
- For every transition update the driver's snapshot and the BSD link state **before** posting: up = SSID, BSSID, LINK up; down =
  LINK down, SSID, BSSID, and the getters then answer no network. Signal-quality updates while up may reuse LINK_CHANGED without
  flapping the link. Which single event repaints the menu is **inferred** — the plan satisfies the whole coherent set (a
  successful join result, RSN completion for PSK, an active link, RUN, agreeing current-network getters) rather than one guessed
  event.

### 8.4 Two ways WPA2 can run

**A — ours (compatibility mode).** airportd only picks the network: ASSOCIATE gives us the SSID, an optional BSSID and the
credential, which we hand to our existing supplicant as an ephemeral per-generation credential (never to the profile file, never
to the logs; the supplicant rereads only the file today, so it needs a session-credential handoff). EAPOL stays on our private
control port; profile-driven auto-join is disabled for the menu-directed generation so it cannot race the pick; a same-name
request still authenticates with the newly supplied credential, never a synthetic success. After our KEY/AUTHORIZE completes and
firmware accepts, set RUN and publish the connected set. This is demonstrated by our sheng binding on 24A446; whether 27.3
airportd tolerates it — refraining from starting eapolclient in every PSK case — is **inferred**. If CIPHER_KEY arrives or Apple
EAPOL starts while our handshake runs, stop the experiment rather than acknowledging keys we did not install: only one
supplicant may own it.

**B — Apple's (the native target).** airportd and eapolclient own WPA2: we associate, keep the data port closed, eapolclient
exchanges EAPOL over its AF_NDRV tap on enX, and installs PTK/GTK through CIPHER_KEY, which we translate to our WMI key
installation, then authorize, raise the link and post the completion events. Two changes at the Ethernet boundary are required:
EAPOL received before authorization must reach enX's input path (today it goes only to our control port), and
`PianoWifiEthernet::outputPacket` must recognize an Ethernet-II EAPOL frame and route it to the existing pre-authorization EAPOL
transmit path while `fLinkUp` is false, with all other data still blocked — the tap must work while the advertised link is down,
so this is not fixed by raising the data link early. Our private key contract becomes a second frontend to the same key
operation, not a parallel implementation: the ioctl may return once the key is validated and queued, completion waits for the
firmware, generation checks reject keys of a previous association, and a firmware failure produces no RSN event and no link-up.
B is the long-term default — it follows the Apple lifecycle the 27.3 facts show and leaves room for enterprise extensions later;
A stays as the fallback and the A/B control.

### 8.5 Staged milestones, each with its one-boot test

**Entry gate:** the adoption question is settled on the tablet first (wm2's mode sweep, §6.2–§6.3): until CoreWiFi reports our
interface in `wdutil` and CoreWLAN scans through, M2 cannot be validated. The gate also requires the logging of §7, host
fixtures for the exact 656/148/16/264/540/32-byte candidates and for malformed lengths/IEs (each inferred fixture marked with its
provenance so a tablet observation replaces it without rewriting the state machine), and the Ethernet path kept as a run-time
A/B control (`piano-wifi-apple80211`, §5); Apple-directed auto-join and our profile auto-join are mutually exclusive per
generation.

- **M2a — the menu shows a join we started ourselves** (no menu-initiated association). A structured association snapshot from
  PianoWifi (generation, raw SSID, BSSID, channel, RSSI/PHY, security, associated/authorized, disconnect reason) replaces M1's
  hard-coded `ENOENT`/INIT answers for STATE, SSID, BSSID, CHANNEL, RSSI, PHY_MODE, RATE, CURRENT_NETWORK, AUTH_TYPE and
  ASSOCIATE_RESULT; the existing authorized-link hook posts the transitions after the snapshot updates; do not synthesize
  ASSOC_DONE for a join airportd did not request; the snapshot is cleared before down events and key wipe, and a joined station
  is not taken off channel (that would lose EAPOL and data). *One boot answers:* with the usual profile join the menu and `wdutil`
  name the current network with its signal, CoreWLAN's current-network query matches our station snapshot, DHCP/data keep working
  through the existing authorized path, a loss or leave produces exactly one down transition and a clean disconnected menu with no
  stale SSID/BSSID; anything airportd initiates by itself (a DISASSOCIATE, its own association) is logged and handled
  deliberately, not hidden as a display success.
- **M2b-A — menu-directed open, then PSK, with our supplicant.** Open first: DISASSOCIATE and ASSOCIATE with no credential,
  asynchronous join, the pending/success/failure result through request 21, the ASSOC_DONE candidate, immediate peer
  authorization, connected getters and events — this validates the whole public join state machine without EAPOL or keys. Then
  WPA2-PSK: the credential handoff of §8.4A, public success delayed until AUTHORIZE, passphrase and PMK forms only with anything
  else rejected; switching, cancellation, timeout, wrong-key failure, duplicate completion and stale-generation callbacks are
  modelled on the host first. *One boot answers:* picking an open network and then a PSK network in the menu joins each, the menu
  reports connected, DHCP and data work, and the log says whether airportd started eapolclient or sent CIPHER_KEY — if it did,
  that is recorded and we move to M2b-B rather than adding a no-op CIPHER_KEY success.
- **M2b-B — the native eapolclient menu join** (the completion target even when A passes first). Reuse the same ASSOCIATE/state/
  event machine, but emit ASSOC_DONE before WPA key completion; our supplicant and profile own nothing in that generation; EAPOL
  routes through enX/NDRV with §8.4B's TX gate; real 148-byte CIPHER_KEY parsing with the cipher translation, PTK/GTK sequencing,
  generation checks, asynchronous WMI completion and key wiping; after GTK plus authorization post RSN_HANDSHAKE_DONE and the
  connected set, set RUN and activate the BSD link (open networks keep the no-EAPOL path); the teardown and failure paths —
  DISASSOCIATE, association failure, EAPOL timeout or cancellation, key-install failure, late events from an old generation —
  exist before the milestone is called complete. *One boot answers:* an airportd/eapolclient PSK join shows the PTK and GTK
  CIPHER_KEY requests and no private-supplicant activity, the keys install, RSN completes, the menu and CoreWLAN show connected,
  DHCP and data work; a repeated join and a disconnect leak no old keys and report no stale current network.

### 8.6 The questions the first M2 boot answers

These share one image/boot once M2b-A reaches the tablet (this section was written with no tablet access):

1. Did the menu use the 656-byte ASSOCIATE directly, call `Apple80211Associate2`, or send a different length?
2. Did AUTH_TYPE and RSN_IE arrive as separate sets, inline only, or both?
3. Was ASSOC_DONE consumed with the 540-byte payload, and was request 21 polled before or after it?
4. Did airportd start eapolclient for PSK right after the 802.11 association, only after LINK_CHANGED, or not at all?
5. For the PTK and GTK CIPHER_KEY, what were flags, index, cipher and RSC length (metadata only)?
6. Was RSN_HANDSHAKE_DONE required once, after each key, or ignored in favour of EAP status plus link and state?
7. Did LINK_CHANGED need the 32-byte inline payload, an empty event plus GET 156, or either?

### 7.8 wm3c: the override in its production form

`out/ram-k273-wm3c` (built from branch commit `19abd9e`, main merged; the 0066 kernel): 237281280 bytes, sha256
`5f5e7ec093cc1e1df58934b942a19cc7c70eaefd41241a0a426c27a0284825cf`, `verify-ram-images.py` PASS; collection `b8e53fe2...` (26 native
kexts all this repository's, the vtable check clean); the v5 recipe with `data-volatile-reserve-mib=1024`, the v2 seed volatile,
the v4 trust cache. Dump image `out/ram-k273-wm3c-dump` (sha256 `ece0cf242dbc2f2ad3ecb1cff35707df5ba608e1a0c7aee68e4a6d77e4c284a8`).

What it answers: whether the override set **before the interface registers** (the default now) makes the **first** airportd adopt
en2 at boot, with no kickstart. Its checklist is §7.4's with stage 0 now meaning the override on:

| Step | Command / line | Read |
|---|---|---|
| boot | `NativeRamBlock: volatile: ... 1024 MiB reserved at start`; `PianoWifi80211: user-client mode 0x8 ...`; the identity line with `IOClassNameOverride IO80211Controller` already at the filter's attach | the production form in effect |
| 2 min after the registration, no kickstart | A (the `[iflist] Successfully created CWFApple80211 (en2)` / `WiFi network interface added (en2)` lines from airportd's first pid), W, I, a scan step | adoption at the first start |
| the control on the same boot | `sudo sysctl machdep.piano_wifi_a80211_uc=0`, kickstart -k, 60 s, A, W | `Did NOT find matching WiFi interface` again (the override is what adopts) |
| the end | console `reset fastboot` | — |
