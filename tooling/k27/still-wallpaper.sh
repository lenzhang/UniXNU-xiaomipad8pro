#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
# The test images' still wallpaper, the power-saving default (docs/drivers/gpu-userspace.md, "Allocation limits and the video wallpaper,
# 2026-11-12"): staged by tooling/k27/build-desktop-images.sh and tooling/k27/build-gfx-test-images.sh (DESK_WALLPAPER=still, the
# default) as /private/var/piano/still-wallpaper.sh with its files (tooling/k27/still-wallpaper.py) in /private/var/piano/wallpaper,
# and run as root by the seed's boot job after settime.sh. SecurityAgent reads the login window's picture about 7 s after settime.sh
# (run 20261112-104004). Without the picture it plays the default aerial wallpaper, a 4096x2160 video decoded in software.
#  - /private/var/db/Wallpapers/<GeneratedUID>/Wallpaper.png and Metadata.plist, root's, as wallpaperexportd leaves them: the picture
#    the login window shows for that account, for every local account (uid 501 or more; the GeneratedUID read from its record in the
#    local directory node, which only root may read, at boot: it is not known when the seed is staged) and for root's and
#    _securityagent's own ids (the login window's fallback for its process's uid).
#  - The account's wallpaper store ~/Library/Application Support/com.apple.wallpaper/Store/Index.plist, the same picture as its
#    choice, so the desktop after the login is still as well: only where the account has no store yet.
#  - The picture itself at /Library/Desktop Pictures/UniXNU Still.png, which the choices name.
# Nothing that exists is replaced: a choice made on the tablet, kept by a persistent Data store, stays. One line per account on the
# console (stderr-log=1): "still-wallpaper: uid 501: login picture written, store written".
S=/private/var/piano/wallpaper
W=/private/var/db/Wallpapers
P="/Library/Desktop Pictures/UniXNU Still.png"
[ -f "$S/still.png" ] && [ -f "$S/Metadata.plist" ] && [ -f "$S/Index.plist" ] || { echo "still-wallpaper: no files in $S"; exit 0; }
install -d -o root -g wheel -m 755 "/Library/Desktop Pictures" "$W"
[ -f "$P" ] || install -o root -g wheel -m 644 "$S/still.png" "$P"
# The login window's picture of one GeneratedUID: "written", or "kept" when one is there already.
put() {
  [ -f "$W/$1/Wallpaper.png" ] && { echo kept; return; }
  install -d -o root -g wheel -m 755 "$W/$1" && install -o root -g wheel -m 644 "$S/still.png" "$W/$1/Wallpaper.png" &&
    install -o root -g wheel -m 644 "$S/Metadata.plist" "$W/$1/Metadata.plist" && echo written || echo failed
}
r=$(put FFFFEEEE-DDDD-CCCC-BBBB-AAAA00000000); a=$(put FFFFEEEE-DDDD-CCCC-BBBB-AAAA0000005C)
echo "still-wallpaper: root and _securityagent: login picture $r, $a"
for f in /private/var/db/dslocal/nodes/Default/users/*.plist; do
  u=$(plutil -extract uid.0 raw -o - "$f" 2>/dev/null) || continue
  [ "$u" -ge 501 ] 2>/dev/null || continue
  g=$(plutil -extract generateduid.0 raw -o - "$f" 2>/dev/null) || continue
  h=$(plutil -extract home.0 raw -o - "$f" 2>/dev/null) || h=
  login=$(put "$(echo "$g" | tr a-f A-F)")
  store=none
  L="$h/Library/Application Support"
  if [ -n "$h" ] && [ -d "$h/Library" ]; then
    if [ -e "$L/com.apple.wallpaper/Store/Index.plist" ]; then store=kept
    else
      gid=$(plutil -extract gid.0 raw -o - "$f" 2>/dev/null) || gid=20   # the account's primary group (staff)
      [ -d "$L" ] || install -d -o "$u" -g "$gid" -m 700 "$L"
      install -d -o "$u" -g "$gid" -m 755 "$L/com.apple.wallpaper" "$L/com.apple.wallpaper/Store" &&
        install -o "$u" -g "$gid" -m 644 "$S/Index.plist" "$L/com.apple.wallpaper/Store/Index.plist" && store=written || store=failed
    fi
  fi
  echo "still-wallpaper: uid $u: login picture $login, store $store"
done
exit 0
