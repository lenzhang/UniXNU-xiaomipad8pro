// SPDX-License-Identifier: BSD-2-Clause-Patent
// pianousb-shell: a login shell for the Mac on PianoDwc3's port 1 (docs/drivers/usb-device-debug.md section 6).
// Runs on the tablet's macOS (a LaunchDaemon, com.unixnu.pianousb-shell.plist): opens /dev/pianousb (the kernel creates it
// with piano-usb-shell=1), starts the shell on a pseudo-terminal and copies bytes both ways. A new shell starts whenever the
// last one exits; /dev/pianousb is reopened if it goes away.
//   pianousb-shell [DEVICE [SHELL]]      defaults: /dev/pianousb /bin/zsh
// Build on the Mac (arm64, ad-hoc signed by the linker): clang -O2 -Wall -Wextra -o pianousb-shell pianousb-shell.c
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <util.h>

static int copy(int from, int to)
{
  char buf[4096];
  const ssize_t n = read(from, buf, sizeof buf);
  if (n <= 0) return n == 0 || (errno != EINTR && errno != EAGAIN) ? -1 : 0;
  for (ssize_t done = 0; done < n;) {
    const ssize_t w = write(to, buf + done, (size_t)(n - done));
    if (w < 0) {
      if (errno == EINTR || errno == EAGAIN) { usleep(1000); continue; }
      return -1;
    }
    done += w;
  }
  return 1;
}

int main(int argc, char **argv)
{
  const char *device = argc > 1 ? argv[1] : "/dev/pianousb", *shell = argc > 2 ? argv[2] : "/bin/zsh";
  signal(SIGPIPE, SIG_IGN);
  for (;;) {
    const int fd = open(device, O_RDWR | O_NOCTTY);
    if (fd < 0) {
      sleep(2);
      continue;
    }
    fprintf(stderr, "pianousb-shell: %s open\n", device);
    for (;;) {
      struct winsize ws = {.ws_row = 50, .ws_col = 160};
      int master = -1;
      const pid_t pid = forkpty(&master, NULL, NULL, &ws);
      if (pid < 0) { sleep(1); break; }
      if (pid == 0) {
        setenv("TERM", "xterm-256color", 1);
        setenv("HOME", "/var/root", 0);
        chdir("/var/root");
        execl(shell, shell, "-l", (char *)NULL);
        execl("/bin/sh", "sh", "-l", (char *)NULL);
        _exit(127);
      }
      static const char banner[] = "\r\npianousb-shell: a new shell on the tablet (exit starts another)\r\n";
      (void)write(fd, banner, sizeof banner - 1);
      int deviceGone = 0;
      for (;;) {
        struct pollfd p[2] = {{.fd = fd, .events = POLLIN}, {.fd = master, .events = POLLIN}};
        if (poll(p, 2, -1) < 0) {
          if (errno == EINTR) continue;
          break;
        }
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
          if (copy(fd, master) < 0) { deviceGone = 1; break; }
        }
        if (p[1].revents & (POLLIN | POLLHUP | POLLERR)) {
          if (copy(master, fd) < 0) break;   // the shell exited
        }
      }
      close(master);
      kill(pid, SIGHUP);
      waitpid(pid, NULL, 0);
      if (deviceGone) break;
    }
    close(fd);
    sleep(1);
  }
}
