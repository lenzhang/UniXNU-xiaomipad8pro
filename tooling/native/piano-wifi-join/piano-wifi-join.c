// SPDX-License-Identifier: BSD-2-Clause-Patent
// piano-wifi-join: hands PianoWifi the network to join, once, from a root-only file (docs/drivers/wifi.md §13.2).
//
//   /usr/local/libexec/piano-wifi-join [FILE]      (FILE defaults to /private/var/db/piano-wifi/profile)
//
// Run as root at boot by the LaunchDaemon com.unixnu.piano-wifi-join. The file is the owner's and stays on the tablet: it must
// be a regular file owned by root with no group or other permission bits (like an ssh key), in a directory owned by root with
// none either; anything else is refused without reading it. Its lines, `key=value`, value to the end of the line:
//   ssid=NAME            the network's name, 1 to 32 bytes, as it is (no quoting)
//   security=open        or wpa2-psk
//   psk=PASSPHRASE       for wpa2-psk: 8 to 63 printable characters, or 64 hex digits
//   bssid=aa:bb:cc:dd:ee:ff   optional: the access point to prefer
//   mac=02:..:..:..:..:..     optional: the station's own address (locally administered)
// Lines starting with # are comments; every line ends with a newline, the last one too (a file still being written is
// refused: replace the file by a rename). The profile goes into the kernel through the sysctl machdep.piano_wifi_join — never a
// command-line argument, never a log line: this program prints only what was wrong, or the lengths it handed over — and every
// copy of it in this process is wiped before it exits. With no file, or no PianoWifi (the sysctl is absent without
// piano-wifi=6 or higher), it exits quietly with 0.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "../../../package/PianoWifi/WifiProfile.h"

static void wipe(void *p, size_t n)
{
  volatile unsigned char *at = (volatile unsigned char *)p;
  while (n--) *at++ = 0;
}

// The file's permissions, checked on the open descriptor and on its directory before a byte is read.
static int safe_open(const char *path)
{
  char dir[1024];
  const size_t len = strlen(path);
  if (len >= sizeof dir) return -1;
  memcpy(dir, path, len + 1);
  char *slash = strrchr(dir, '/');
  if (!slash || slash == dir) return -1;
  *slash = 0;
  struct stat ds;
  if (lstat(dir, &ds) != 0) return -2;
  if (!S_ISDIR(ds.st_mode) || ds.st_uid != 0 || (ds.st_mode & 077)) {
    fprintf(stderr, "piano-wifi-join: the profile's directory must be root's with mode 700; not read\n");
    return -1;
  }
  const int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return errno == ENOENT ? -2 : -1;
  struct stat fs;
  if (fstat(fd, &fs) != 0 || !S_ISREG(fs.st_mode) || fs.st_uid != 0 || (fs.st_mode & 077) || fs.st_size > 4096) {
    fprintf(stderr, "piano-wifi-join: the profile must be a regular file of root's with mode 600, at most 4 KiB; not read\n");
    close(fd);
    return -1;
  }
  return fd;
}

int main(int argc, char **argv)
{
  const char *path = argc > 1 ? argv[1] : PIANO_WIFI_PROFILE_PATH;
  if (geteuid() != 0) {
    fprintf(stderr, "piano-wifi-join: run as root\n");
    return 1;
  }
  // No PianoWifi in this boot (or not at a level that joins): nothing to do. launchd may start this before the kext has
  // registered its sysctl, so it is looked for during 30 s before that conclusion.
  size_t probe = 0;
  int tries = 30;
  while (sysctlbyname(PIANO_WIFI_PROFILE_SYSCTL, NULL, &probe, NULL, 0) != 0) {
    if (--tries <= 0) return 0;
    sleep(1);
  }
  const int fd = safe_open(path);
  if (fd == -2) return 0;   // no profile: nothing to join
  if (fd < 0) return 1;
  static char text[4097];
  const ssize_t got = read(fd, text, sizeof text - 1);
  close(fd);
  if (got <= 0) {
    wipe(text, sizeof text);
    return 1;
  }
  // The shared parser (WifiProfile.h): the kext's and this helper's rules are one, the supplicant's the same (a last line
  // without its newline is a file still being written: refused, review of 2026-11-14 R1/R2).
  struct piano_wifi_profile p;
  const char *problem = piano_wifi_profile_parse((const uint8_t *)text, (uint32_t)got, &p);
  wipe(text, sizeof text);
  if (problem) {
    fprintf(stderr, "piano-wifi-join: the profile is not usable: %s; nothing handed over\n", problem);
    piano_wifi_profile_wipe(&p);
    return 1;
  }
  const int rc = sysctlbyname(PIANO_WIFI_PROFILE_SYSCTL, NULL, NULL, &p, sizeof p);
  const int err = errno;
  const unsigned ssid = p.ssid_len, psk = p.psk_len;
  const char *security = piano_wifi_security_name(p.security);
  piano_wifi_profile_wipe(&p);
  if (rc != 0) {
    fprintf(stderr, "piano-wifi-join: the kernel refused the profile (%s)\n", strerror(err));
    return 1;
  }
  if (psk)
    printf("piano-wifi-join: handed over a network name of %u byte(s), %s with a passphrase of %u character(s)\n", ssid,
           security, psk);
  else
    printf("piano-wifi-join: handed over a network name of %u byte(s), %s\n", ssid, security);
  return 0;
}
