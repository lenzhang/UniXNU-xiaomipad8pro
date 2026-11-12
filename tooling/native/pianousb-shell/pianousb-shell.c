// SPDX-License-Identifier: BSD-2-Clause-Patent
// pianousb-shell: a login shell for the Mac on PianoDwc3's port 1 (docs/drivers/usb-device-debug.md section 6).
// Runs on the tablet's macOS (a LaunchDaemon, com.unixnu.pianousb-shell.plist): opens /dev/pianousb (the kernel creates it
// with piano-usb-shell=1), starts the shell on a pseudo-terminal and copies bytes both ways. A new shell starts whenever the
// last one exits; /dev/pianousb is reopened if it goes away.
//   pianousb-shell [DEVICE [SHELL]]      defaults: /dev/pianousb /bin/zsh
// On /dev/pianousb it runs only when the kernel was booted with piano-usb-shell=1 (the argument that makes the kernel create the
// node): without it, it exits at once with status 0 and its job (KeepAlive SuccessfulExit false) is not started again, so an
// installed profile without the argument has no shell and no polling for the node. Another DEVICE (a test pty) skips the check.
// Waits with select(), never poll(): XNU's poll() is kqueue underneath, and a character device's knote attaches only when its
// cdevsw is marked kqueue-capable (spec_kqfilter, filt_specattach: EINVAL otherwise) or the caller asks NOTE_LOWAT 1, which poll()
// never does; so poll() on /dev/pianousb as k27 built it returns POLLNVAL at once, every time. The helper of 2026-11-12 polled and
// took only POLLIN/POLLHUP/POLLERR: it spun at full CPU ("pianousb-shell caught burning CPU", desk-dev 17:27) and never read a byte
// the Mac sent, so the shell got no input and sent nothing back. select() goes to the cdevsw's d_select (selscan, fo_select) for
// any device; PianoDwc3 now also marks its cdevsw kqueue-capable. Host test: test_pianousb_shell.py (a device whose poll() answers
// POLLNVAL, as /dev/pianousb's did).
// Build on the Mac (arm64, ad-hoc signed by the linker): clang -O2 -Wall -Wextra -o pianousb-shell pianousb-shell.c
// Installed by configs/userland.toml (tooling/userland/stage.py, docs/userland.md).
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <util.h>

// piano-usb-shell=N in kern.bootargs with N not 0 (PianoDwc3 creates /dev/pianousb for any nonzero number).
static int shellArgument(void)
{
  char args[2048];
  size_t size = sizeof args - 1;
  if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) != 0) return 0;
  args[size] = 0;
  static const char key[] = "piano-usb-shell=";
  for (char *p = args; (p = strstr(p, key)) != NULL; p += sizeof key - 1) {
    if (p != args && p[-1] != ' ') continue;
    return strtoul(p + sizeof key - 1, NULL, 0) != 0;
  }
  return 0;
}

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
  if (strcmp(device, "/dev/pianousb") == 0 && !shellArgument()) {
    fprintf(stderr, "pianousb-shell: no piano-usb-shell=1 in the boot arguments: no %s, nothing to do\n", device);
    return 0;
  }
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
      if (fd >= FD_SETSIZE || master >= FD_SETSIZE) deviceGone = 1;   // never with launchd's few descriptors
      while (!deviceGone) {
        fd_set r;
        FD_ZERO(&r);
        FD_SET(fd, &r);
        FD_SET(master, &r);
        const int n = select((fd > master ? fd : master) + 1, &r, NULL, NULL, NULL);
        if (n < 0) {
          if (errno == EINTR) continue;
          fprintf(stderr, "pianousb-shell: select: %s: %s reopened\n", strerror(errno), device);
          deviceGone = 1;   // a descriptor went bad: start again from the open
          break;
        }
        if (FD_ISSET(fd, &r) && copy(fd, master) < 0) { deviceGone = 1; break; }
        if (FD_ISSET(master, &r) && copy(master, fd) < 0) break;   // the shell exited
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
