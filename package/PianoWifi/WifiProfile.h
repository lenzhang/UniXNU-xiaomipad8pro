// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoWifi level 7's join profile: the network to join and how, as it crosses from the tablet's root-only file into the
// kernel (docs/drivers/wifi.md §13.2). New code for this repository.
//
// The path: /private/var/db/piano-wifi/profile (root:wheel, mode 600, directory 700; the owner places it, never a repository)
// is read once at boot by /usr/local/libexec/piano-wifi-join (the LaunchDaemon com.unixnu.piano-wifi-join), which packs it into
// the structure below and writes it into the sysctl machdep.piano_wifi_join — root only (no CTLFLAG_ANYBODY), never an argument
// on a command line — and then wipes its own copy. A read of the sysctl returns a status line with lengths only. The kernel
// keeps the profile in its own memory and clears it when the radio stops; no byte of a name, an address or a passphrase is
// ever logged, only their lengths and whether they were given.
//
// This header is shared by the kext and the helper (C and C++), so both check the same rules.
#pragma once
#include <stdint.h>

#define PIANO_WIFI_PROFILE_MAGIC 0x314a5750u   /* "PWJ1" */
#define PIANO_WIFI_PROFILE_VERSION 1u
#define PIANO_WIFI_PROFILE_SYSCTL "machdep.piano_wifi_join"
#define PIANO_WIFI_PROFILE_PATH "/private/var/db/piano-wifi/profile"

enum { PianoWifiSecOpen = 0, PianoWifiSecWpa2Psk = 1 };
enum { PianoWifiProfileBssid = 1u << 0, PianoWifiProfileMac = 1u << 1 };

struct piano_wifi_profile {
  uint32_t magic, version, flags;
  uint8_t security;       // PianoWifiSec*
  uint8_t ssid_len;       // 1..32
  uint8_t psk_len;        // 0 for an open network; 8..63 for a passphrase, 64 for a key in hex
  uint8_t reserved;
  uint8_t ssid[32];
  uint8_t bssid[6];       // with PianoWifiProfileBssid: the access point to prefer
  uint8_t mac[6];         // with PianoWifiProfileMac: the address the station takes (locally administered)
  uint8_t psk[64];
};

// What is wrong with a profile, or 0. The checks are the ones a supplicant makes: a name of 1 to 32 bytes, a passphrase of 8
// to 63 printable characters or 64 hex digits for WPA2-PSK and none for an open network, an address of the station that is
// locally administered and not a group address. Nothing else of the profile is looked at.
static inline const char *piano_wifi_profile_problem(const struct piano_wifi_profile *p, uint32_t size)
{
  uint32_t i;
  if (!p || size != sizeof *p) return "a profile of the wrong size";
  if (p->magic != PIANO_WIFI_PROFILE_MAGIC || p->version != PIANO_WIFI_PROFILE_VERSION) return "an unknown profile version";
  if (p->ssid_len < 1 || p->ssid_len > 32) return "a network name that is not 1 to 32 bytes";
  if (p->security == PianoWifiSecOpen) {
    if (p->psk_len) return "a passphrase for an open network";
  } else if (p->security == PianoWifiSecWpa2Psk) {
    if (p->psk_len == 64) {
      for (i = 0; i < 64; ++i) {
        const uint8_t c = p->psk[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return "a 64-character key that is not hex";
      }
    } else {
      if (p->psk_len < 8 || p->psk_len > 63) return "a passphrase that is not 8 to 63 characters";
      for (i = 0; i < p->psk_len; ++i)
        if (p->psk[i] < 0x20 || p->psk[i] > 0x7e) return "a passphrase with a character that is not printable ASCII";
    }
  } else {
    return "an unknown security kind";
  }
  if (p->flags & ~(uint32_t)(PianoWifiProfileBssid | PianoWifiProfileMac)) return "unknown profile flags";
  if (p->flags & PianoWifiProfileBssid) {
    uint32_t zero = 1;
    for (i = 0; i < 6; ++i) zero = zero && !p->bssid[i];
    if (zero || (p->bssid[0] & 1)) return "an access point address that is zero or a group address";
  }
  if ((p->flags & PianoWifiProfileMac) && ((p->mac[0] & 3) != 2)) return "a station address that is not locally administered";
  return 0;
}

static inline const char *piano_wifi_security_name(uint8_t security)
{
  return security == PianoWifiSecOpen ? "open" : security == PianoWifiSecWpa2Psk ? "WPA2-PSK" : "unknown";
}

// Wipes a profile so that the compiler cannot drop the stores (the helper's copy and the kernel's when the radio stops).
static inline void piano_wifi_profile_wipe(struct piano_wifi_profile *p)
{
  volatile uint8_t *at = (volatile uint8_t *)p;
  uint32_t i;
  if (!p) return;
  for (i = 0; i < sizeof *p; ++i) at[i] = 0;
}
