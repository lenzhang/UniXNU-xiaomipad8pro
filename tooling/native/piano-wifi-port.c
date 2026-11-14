/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* piano-wifi-port: a listener on PianoWifi's 802.1X port (docs/drivers/wifi.md section 13.7), for the tablet itself. It
 * connects to the kernel control com.unixnu.driver.PianoWifi.8021x (root only; piano-wifi=8), asks for the LINK message, and
 * prints every message the kext sends: its type, length, the association's state and generation, and for EAPOL frames which
 * message of the handshake it is and its length. Never an address, a frame's bytes or a key: what it prints may be pasted.
 * It answers nothing, so the access point's handshake still times out; it shows that the port carries the frames up.
 *
 *   piano-wifi-port [SECONDS]     listen (default 300 s), then exit; 0 = until interrupted
 *
 * Build on the Mac (macOS SDK, arm64):  clang -O2 -Wall -Wextra -I package/PianoWifi tooling/native/piano-wifi-port.c \
 *                                         -o out/piano-wifi-port
 * then copy it to the tablet and run it as root after the boot (before the join profile, or at any time: a LINK message
 * comes back on connect). */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/kern_control.h>
#include <sys/socket.h>
#include <sys/sys_domain.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "PianoWifiNet.h"

static double since(const struct timespec *t0)
{
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)(t.tv_sec - t0->tv_sec) + (double)(t.tv_nsec - t0->tv_nsec) / 1e9;
}

static const char *stateName(uint32_t s)
{
  return s == PIANO_WIFI_8021X_STATE_ASSOCIATED ? "associated (port closed)"
         : s == PIANO_WIFI_8021X_STATE_AUTHORIZED ? "authorized" : "idle";
}

/* which message of the handshakes an EAPOL-Key PDU is (the key information bits; as WifiNet.h netEapolKind) */
static const char *eapolName(const uint8_t *pdu, uint32_t n)
{
  if (n < 4) return "too short";
  if (pdu[1] != 3) return "not a key frame";
  if (n < 99) return "a key frame cut short";
  const uint32_t info = (uint32_t)pdu[5] << 8 | pdu[6];
  const int pairwise = (info >> 3) & 1, install = (info >> 6) & 1, ack = (info >> 7) & 1, mic = (info >> 8) & 1,
            secure = (info >> 9) & 1;
  if (pairwise) {
    if (ack && !mic) return "four-way message 1";
    if (ack && mic && install) return "four-way message 3";
    if (!ack && mic && !secure) return "four-way message 2";
    if (!ack && mic && secure) return "four-way message 4";
  } else {
    if (ack && mic) return "group message 1";
    if (!ack && mic) return "group message 2";
  }
  return "another key frame";
}

int main(int argc, char **argv)
{
  const long seconds = argc > 1 ? strtol(argv[1], NULL, 10) : 300;
  const int s = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
  if (s < 0) {
    perror("piano-wifi-port: socket");
    return 1;
  }
  struct ctl_info info;
  memset(&info, 0, sizeof info);
  strlcpy(info.ctl_name, PIANO_WIFI_8021X_CONTROL, sizeof info.ctl_name);
  if (ioctl(s, CTLIOCGINFO, &info) < 0) {
    fprintf(stderr, "piano-wifi-port: no control %s (%s): is piano-wifi=8 running?\n", PIANO_WIFI_8021X_CONTROL,
            strerror(errno));
    return 1;
  }
  struct sockaddr_ctl sc;
  memset(&sc, 0, sizeof sc);
  sc.sc_len = sizeof sc;
  sc.sc_family = AF_SYSTEM;
  sc.ss_sysaddr = AF_SYS_CONTROL;
  sc.sc_id = info.ctl_id;
  sc.sc_unit = 0;
  if (connect(s, (struct sockaddr *)&sc, sizeof sc) < 0) {
    fprintf(stderr, "piano-wifi-port: connect: %s%s\n", strerror(errno),
            errno == EBUSY ? " (another client is connected)" : errno == EPERM ? " (root only)" : "");
    return 1;
  }
  printf("piano-wifi-port: connected to %s (id %u)\n", PIANO_WIFI_8021X_CONTROL, info.ctl_id);
  /* ask for the LINK message (the kext sends one on connect too) */
  struct {
    struct piano_wifi_8021x_hdr h;
    struct piano_wifi_8021x_request r;
  } __attribute__((packed)) q;
  memset(&q, 0, sizeof q);
  q.h.type = PIANO_WIFI_8021X_LINK_QUERY;
  q.h.length = sizeof q;
  q.h.seq = 1;
  if (send(s, &q, sizeof q, 0) < 0) perror("piano-wifi-port: send LINK_QUERY");
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  if (seconds > 0) {
    struct timeval tv = {1, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  }
  static uint8_t m[PIANO_WIFI_8021X_MSG_MAX];
  unsigned counts[4] = {0};
  for (;;) {
    if (seconds > 0 && since(&t0) > (double)seconds) break;
    const ssize_t n = recv(s, m, sizeof m, 0);
    if (n < 0) {
      if (errno == EAGAIN || errno == EINTR) continue;
      perror("piano-wifi-port: recv");
      break;
    }
    if (n < (ssize_t)sizeof(struct piano_wifi_8021x_hdr)) {
      printf("%8.3f  a message of %zd bytes (shorter than the header)\n", since(&t0), n);
      continue;
    }
    struct piano_wifi_8021x_hdr h;
    memcpy(&h, m, sizeof h);
    const uint8_t *body = m + sizeof h;
    const size_t bodyLength = (size_t)n - sizeof h;
    if (h.length != n) printf("%8.3f  the header says %u bytes, the message has %zd\n", since(&t0), h.length, n);
    if (h.type < 4) ++counts[h.type];
    if (h.type == PIANO_WIFI_8021X_LINK && bodyLength >= sizeof(struct piano_wifi_8021x_link)) {
      struct piano_wifi_8021x_link l;
      memcpy(&l, body, sizeof l);
      printf("%8.3f  LINK: %s, generation %u, AID %u, %u MHz, security %u (pairwise %u group %u AKM %u), RSN elements %u/%u "
             "bytes, reason %u\n",
             since(&t0), stateName(l.state), l.generation, l.aid, l.freq, l.security, l.pairwise, l.group, l.akm, l.ap_rsn_len,
             l.own_rsn_len, l.reason);
    } else if (h.type == PIANO_WIFI_8021X_EAPOL_RX && bodyLength >= sizeof(struct piano_wifi_8021x_eapol)) {
      struct piano_wifi_8021x_eapol e;
      memcpy(&e, body, sizeof e);
      const uint8_t *pdu = body + sizeof e;
      const uint32_t have = (uint32_t)(bodyLength - sizeof e);
      printf("%8.3f  EAPOL_RX: generation %u, PDU %u bytes (%u here), %s\n", since(&t0), e.generation, e.pdu_len, have,
             eapolName(pdu, have < e.pdu_len ? have : e.pdu_len));
    } else if (h.type == PIANO_WIFI_8021X_RESULT && bodyLength >= sizeof(struct piano_wifi_8021x_result)) {
      struct piano_wifi_8021x_result r;
      memcpy(&r, body, sizeof r);
      printf("%8.3f  RESULT: request %u seq %u: status %d detail %u generation %u\n", since(&t0), r.request, h.seq, r.status,
             r.detail, r.generation);
    } else {
      printf("%8.3f  type %u, %zd bytes\n", since(&t0), h.type, n);
    }
    fflush(stdout);
  }
  printf("piano-wifi-port: %u LINK, %u EAPOL_RX, %u RESULT message(s) in %.0f s\n", counts[1], counts[2], counts[3], since(&t0));
  close(s);
  return 0;
}
