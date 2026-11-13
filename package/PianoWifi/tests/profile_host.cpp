// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host test of level 7's join profile rules (WifiProfile.h), which the kext's sysctl and the tablet's piano-wifi-join both
// apply: what a usable profile is, and that a wipe leaves nothing behind. The profiles here are made up for the test: no
// network of anyone's.
//   c++ -std=c++20 -Wall -Wextra -Werror -I.. profile_host.cpp -o /tmp/profile_host && /tmp/profile_host
#include "WifiProfile.h"
#include <cstdio>
#include <cstring>

static int failures;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static piano_wifi_profile base(uint8_t security)
{
  piano_wifi_profile p;
  std::memset(&p, 0, sizeof p);
  p.magic = PIANO_WIFI_PROFILE_MAGIC;
  p.version = PIANO_WIFI_PROFILE_VERSION;
  p.security = security;
  std::memcpy(p.ssid, "test-network", 12);
  p.ssid_len = 12;
  if (security == PianoWifiSecWpa2Psk) {
    std::memcpy(p.psk, "correct horse", 13);
    p.psk_len = 13;
  }
  return p;
}

int main()
{
  piano_wifi_profile p = base(PianoWifiSecWpa2Psk);
  CHECK(!piano_wifi_profile_problem(&p, sizeof p));
  p = base(PianoWifiSecOpen);
  CHECK(!piano_wifi_profile_problem(&p, sizeof p));
  // the structure itself
  CHECK(piano_wifi_profile_problem(nullptr, sizeof p));
  CHECK(piano_wifi_profile_problem(&p, sizeof p - 1));
  p.magic = 0;
  CHECK(piano_wifi_profile_problem(&p, sizeof p));
  p = base(PianoWifiSecOpen);
  p.version = 2;
  CHECK(piano_wifi_profile_problem(&p, sizeof p));
  // the name
  p = base(PianoWifiSecOpen);
  p.ssid_len = 0;
  CHECK(piano_wifi_profile_problem(&p, sizeof p));
  p.ssid_len = 33;
  CHECK(piano_wifi_profile_problem(&p, sizeof p));
  p.ssid_len = 32;
  CHECK(!piano_wifi_profile_problem(&p, sizeof p));
  // the passphrase: none for an open network; 8 to 63 printable, or 64 hex, for WPA2-PSK
  p = base(PianoWifiSecOpen);
  p.psk_len = 8;
  CHECK(piano_wifi_profile_problem(&p, sizeof p));
  p = base(PianoWifiSecWpa2Psk);
  p.psk_len = 7;
  CHECK(piano_wifi_profile_problem(&p, sizeof p));
  p.psk_len = 0;
  CHECK(piano_wifi_profile_problem(&p, sizeof p));
  std::memset(p.psk, 'a', 63);
  p.psk_len = 63;
  CHECK(!piano_wifi_profile_problem(&p, sizeof p));
  p.psk[10] = 0x07;
  CHECK(piano_wifi_profile_problem(&p, sizeof p));
  std::memset(p.psk, 'F', 64);
  p.psk_len = 64;
  CHECK(!piano_wifi_profile_problem(&p, sizeof p));
  p.psk[63] = 'g';
  CHECK(piano_wifi_profile_problem(&p, sizeof p));
  p = base(7);
  CHECK(piano_wifi_profile_problem(&p, sizeof p));
  // the addresses
  p = base(PianoWifiSecOpen);
  p.flags = PianoWifiProfileBssid;
  CHECK(piano_wifi_profile_problem(&p, sizeof p));   // all zeros
  p.bssid[0] = 0x01;
  CHECK(piano_wifi_profile_problem(&p, sizeof p));   // a group address
  p.bssid[0] = 0x02;
  CHECK(!piano_wifi_profile_problem(&p, sizeof p));
  p.flags |= PianoWifiProfileMac;
  p.mac[0] = 0x00;
  CHECK(piano_wifi_profile_problem(&p, sizeof p));   // a station address that is not locally administered
  p.mac[0] = 0x02;
  CHECK(!piano_wifi_profile_problem(&p, sizeof p));
  p.flags |= 1u << 5;
  CHECK(piano_wifi_profile_problem(&p, sizeof p));
  // the wipe
  p = base(PianoWifiSecWpa2Psk);
  piano_wifi_profile_wipe(&p);
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&p);
  bool clean = true;
  for (unsigned i = 0; i < sizeof p; ++i) clean = clean && !bytes[i];
  CHECK(clean);
  CHECK(std::strcmp(piano_wifi_security_name(PianoWifiSecWpa2Psk), "WPA2-PSK") == 0);
  // the file's text (piano_wifi_profile_parse, shared by the helper and the kext)
  {
    const char text[] = "# the owner's network\r\nssid=my home\r\nsecurity=wpa2-psk\r\npsk=correct horse\r\n\r\n"
                        "bssid=3c:52:a1:10:20:30\n";
    piano_wifi_profile q;
    CHECK(piano_wifi_profile_parse(reinterpret_cast<const uint8_t *>(text), sizeof text - 1, &q) == nullptr);
    CHECK(q.ssid_len == 7 && std::memcmp(q.ssid, "my home", 7) == 0 && q.security == PianoWifiSecWpa2Psk);
    CHECK(q.psk_len == 13 && (q.flags & PianoWifiProfileBssid) && q.bssid[0] == 0x3c && q.bssid[5] == 0x30);
    const char open[] = "ssid=cafe\nsecurity=open";   // no final newline
    CHECK(piano_wifi_profile_parse(reinterpret_cast<const uint8_t *>(open), sizeof open - 1, &q) == nullptr);
    CHECK(q.security == PianoWifiSecOpen && q.psk_len == 0 && q.ssid_len == 4);
    const char *bad[] = {"ssid=x\nsecurity=wep\n", "ssid=x\n", "ssid=x\nsecurity=open\ncolour=blue\n",
                         "ssid=x\nsecurity=wpa2-psk\npsk=short\n", "ssid=x\nsecurity=open\nmac=02:00:00:00:00\n",
                         "ssid=012345678901234567890123456789012\nsecurity=open\n"};
    for (const char *b : bad)
      CHECK(piano_wifi_profile_parse(reinterpret_cast<const uint8_t *>(b), uint32_t(std::strlen(b)), &q) != nullptr);
  }
  std::printf(failures ? "profile_host: %d check(s) failed\n" : "profile_host: all passed\n", failures);
  return failures ? 1 : 0;
}
