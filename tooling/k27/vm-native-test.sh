#!/bin/bash
# Plan item 4: the k27 native-variant kernel collection inside the existing Gunyah VM with an 8-CPU, two-cluster ADT
# (EEEEEEPP), then back to the 1600x1068 direct accept path. One owner-approved session; run the phases in order:
#   vm-native-test.sh stage          copy the test boot disk + symbol map to the tablet, back up the accept boot.img/boot.env
#   vm-native-test.sh switch-test    put the test disk in place (BOOT_CPUS=8) and switch the direct VM to it (macos-direct.sh)
#   vm-native-test.sh check          wait for the login, then collect the evidence (CPUs, clusters, conduit, processes, FDT)
#   vm-native-test.sh switch-accept  put the accept disk back and switch the direct VM to it
#   vm-native-test.sh verify         macos-vm active, charger online, login reachable, accept disk hash
# The tablet's sudo password is read at run time from PIANO_SUDO_PW_FILE (mode 600, outside every repository) and handed to sudo on
# stdin: it is never printed, logged or stored here. Nothing is flashed; only ~/vm/images/macos27/boot.{img,env} change and
# are restored. Never systemctl isolate; piano-charging and piano-thp stay running (macos-direct.sh starts them anyway).
set -euo pipefail
T=${PIANO_HOST:?set PIANO_HOST to user@tablet}
S=${BUILD_HOST:?set BUILD_HOST to user@build-host}
I=vm/images/macos27
ACC=accept-20261110
ACCEPT_SHA=dcbbcc9b72d9b379303b76e4c78d6c979961f9e2210a4c03c676d6a04c2a61bd
D=$(cd "$(dirname "$0")/../.." && pwd)
PWFILE=${PIANO_SUDO_PW_FILE:?set PIANO_SUDO_PW_FILE to a mode-600 file outside the repo}
pw() { cat "$PWFILE"; }
rsudo() { pw | ssh -o BatchMode=yes "$T" "sudo -S -p '' bash -c $(printf %q "$1")"; }
gssh() { ssh -o BatchMode=yes "$T" "~/vm/rig/gssh.sh $(printf %q "$1")"; }
log() { echo "$(date +%T) $*"; }
case ${1:-} in
stage)
  ssh -o BatchMode=yes "$S" "rsync -a --sparse ~/droidvm/work/macos27/boot-native-test/boot.img $T:$I/boot-native-test.img"
  scp -q "$D/out/k27/kc-k27-native.nm" "$T:vm/rig/kernel-k27-native-0054.nm"
  ssh -o BatchMode=yes "$T" "cd $I && { [ -e boot.img.$ACC ] || cp --sparse=always -p boot.img boot.img.$ACC; } && { [ -e boot.env.$ACC ] || cp -p boot.env boot.env.$ACC; } \
    && sha256sum boot.img boot.img.$ACC boot-native-test.img | sed 's/  / /' && cat boot.env.$ACC"
  ;;
switch-test)
  ssh -o BatchMode=yes "$T" "cd $I && [ \"\$(sha256sum boot.img.$ACC | cut -c1-64)\" = $ACCEPT_SHA ] && cp --sparse=always boot-native-test.img boot.img \
    && sed 's/^BOOT_CPUS=.*/BOOT_CPUS=8/' boot.env.$ACC > boot.env && cat boot.env"
  log "switching the direct VM to the test disk"
  rsudo "/home/leo/vm/macos-direct.sh" 2>&1 | tail -3
  ssh -o BatchMode=yes "$T" "systemctl is-active macos-vm piano-charging piano-thp gdm; ls -td ~/vm/runs/*/ | head -1"
  ;;
check|verify)
  want=$([ "$1" = check ] && echo 8 || echo 6)
  log "waiting for the guest login (loginwindow, up to 10 min)"
  for i in $(seq 60); do gssh 'pgrep -x loginwindow >/dev/null && pgrep -x WindowServer >/dev/null' 2>/dev/null && break; sleep 10; done
  log "guest:"
  gssh 'sysctl hw.ncpu hw.activecpu hw.nperflevels hw.perflevel0.name hw.perflevel0.logicalcpu hw.perflevel1.name hw.perflevel1.logicalcpu kern.osversion 2>&1; uptime; for p in loginwindow WindowServer Dock Finder SystemUIServer; do printf "%s %s\n" $p "$(pgrep -x $p | tr "\n" " ")"; done; who' || echo "guest not reachable over ssh"
  R=$(ssh -o BatchMode=yes "$T" "ls -td ~/vm/runs/*/ | head -1")
  log "run $R"
  ssh -o BatchMode=yes "$T" "grep -E '^(cpus|affinity|mem)=' $R/info.txt 2>/dev/null; grep -c 'system reset event' $R/stderr.log 2>/dev/null | sed 's/^/reset events: /'; \
    dtc -q -I dtb -O dts $R/generated.dtb 2>/dev/null | grep -cE '^\s+cpu@' | sed 's/^/FDT cpu nodes: /'; dtc -q -I dtb -O dts $R/generated.dtb 2>/dev/null | grep -A4 'intc {' | grep reg"
  if [ "$1" = check ]; then
    log "conduit in the running native kernel (gpeek, 4 bytes each):"
    rsudo "cd /home/leo/vm/rig && KERNEL_NM=/home/leo/vm/rig/kernel-k27-native-0054.nm python3 gpeek.py _ml_psci_conduit 4; KERNEL_NM=/home/leo/vm/rig/kernel-k27-native-0054.nm python3 gpeek.py _ml_psci_conduit_from_dt 4" || true
  else
    ssh -o BatchMode=yes "$T" "systemctl is-active macos-vm piano-charging piano-thp gdm; for p in /sys/class/power_supply/*/; do printf '%s online=%s\n' \$(basename \$p) \$(cat \$p/online 2>/dev/null); done; \
      cat /sys/class/power_supply/xiaomi-mca-usb/voltage_now /sys/class/power_supply/xiaomi-mca-usb/current_now 2>/dev/null | tr '\n' ' '; echo; sha256sum $I/boot.img | cut -c1-64; cat $I/boot.env"
  fi
  log "expected hw.ncpu $want"
  ;;
switch-accept)
  ssh -o BatchMode=yes "$T" "cd $I && [ \"\$(sha256sum boot.img.$ACC | cut -c1-64)\" = $ACCEPT_SHA ] && cp --sparse=always boot.img.$ACC boot.img && cp -p boot.env.$ACC boot.env \
    && sha256sum boot.img | cut -c1-64 && cat boot.env"
  log "switching the direct VM back to the accept disk"
  rsudo "/home/leo/vm/macos-direct.sh" 2>&1 | tail -3
  log "the test VM's stop (PSCI SYSTEM_OFF through the ADT's /psci method in the native kernel):"
  rsudo "journalctl -t macos-vm-stop --since -10min --no-pager -o cat | tail -6" || true
  ssh -o BatchMode=yes "$T" "systemctl is-active macos-vm piano-charging piano-thp gdm; ls -td ~/vm/runs/*/ | head -1"
  ;;
*) sed -n '2,10p' "$0"; exit 2 ;;
esac
