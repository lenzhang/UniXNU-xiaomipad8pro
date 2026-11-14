# The macOS Wi-Fi menu: PianoWifi under Apple's Wi-Fi stack (design, 2026-11-14)

Status: design in progress; nothing built. The link itself works since boot 10i (2026-11-14: WPA2, DHCP, the internet over
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
