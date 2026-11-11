#!/usr/bin/env bash
# home-ram-boot.sh — Xiaomi Pad 8 Pro (piano) RAM-only boot helper.
# Default: check images, SHA-256, fastboot binary, and whether a device is visible.
# Only `./home-ram-boot.sh --boot` runs `fastboot boot` (into RAM, no partition write).
# This script never flashes. Any flash/flashing argument is rejected.
set -euo pipefail

usage() {
  cat <<'EOF'
用法:
  home-ram-boot.sh              只做检查（默认，不启动设备）
  home-ram-boot.sh --check      同上
  home-ram-boot.sh --list       列出将使用的镜像目录与建议顺序
  home-ram-boot.sh --boot       对已连接的 fastboot 设备执行 RAM 启动（不写分区）
  home-ram-boot.sh --boot --image <文件或别名>

别名（默认 first = uefi-B）:
  uefi-B    boot-piano-uefi-ram-B-own303.img          首选，约 11 MB，未签名
  uefi-A    boot-piano-uefi-ram-A-bundled.img         没画面时的备选
  macos-B   boot-piano-macos-ram-B-own303.img         之后的 macOS RAM 镜像，约 194 MiB，先选 1 核
  macos-A   boot-piano-macos-ram-A-bundled.img

环境变量:
  PIANO_RAM_DIR     镜像目录（含 SHA256SUMS）
  FASTBOOT          fastboot 可执行文件路径
  PIANO_BOOT_LOGDIR 日志目录（默认自动选择）

本脚本拒绝任何 flash / flashing 参数，也绝不会调用 `fastboot flash`。
EOF
}

die() { echo "错误: $*" >&2; exit 1; }
log() { echo "$*"; }

for arg in "$@"; do
  case "$arg" in
    -h|--help) usage; exit 0 ;;
  esac
  # Reject flash in any form, including --flash, flash, --flashing, fastboot-flash
  if printf '%s' "$arg" | grep -qiE 'flash'; then
    echo "拒绝: 本脚本只允许 RAM 启动（fastboot boot），绝不 flash / 写分区。" >&2
    echo "已忽略全部操作，设备未被改动。" >&2
    exit 2
  fi
done

MODE=check
IMAGE_KEY=uefi-B
IMAGE_ARG=""
while [ $# -gt 0 ]; do
  case "$1" in
    --check|--dry-run) MODE=check; shift ;;
    --list) MODE=list; shift ;;
    --boot) MODE=boot; shift ;;
    --image)
      [ $# -ge 2 ] || die "--image 需要一个文件路径或别名"
      IMAGE_ARG="$2"
      shift 2
      ;;
    --image=*)
      IMAGE_ARG="${1#--image=}"
      shift
      ;;
    *)
      die "未知参数: $1  （运行 --help 查看用法；本脚本不接受 flash）"
      ;;
  esac
done

resolve_fastboot() {
  if [ -n "${FASTBOOT:-}" ]; then
    [ -x "$FASTBOOT" ] || die "FASTBOOT=$FASTBOOT 不可执行"
    echo "$FASTBOOT"
    return
  fi
  if command -v fastboot >/dev/null 2>&1; then
    command -v fastboot
    return
  fi
  local c
  for c in \
    /opt/homebrew/bin/fastboot \
    /opt/homebrew/share/android-commandlinetools/platform-tools/fastboot \
    /usr/local/bin/fastboot \
    /usr/local/share/android-commandlinetools/platform-tools/fastboot
  do
    if [ -x "$c" ]; then
      echo "$c"
      return
    fi
  done
  return 1
}

sha256_of() {
  local f="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum -- "$f" | awk '{print $1}'
  else
    shasum -a 256 -- "$f" | awk '{print $1}'
  fi
}

alias_to_name() {
  case "$1" in
    uefi-B|B|b|own303|uefi-own303) echo boot-piano-uefi-ram-B-own303.img ;;
    uefi-A|A|a|bundled|uefi-bundled) echo boot-piano-uefi-ram-A-bundled.img ;;
    macos-B|mac-B|macos-own303) echo boot-piano-macos-ram-B-own303.img ;;
    macos-A|mac-A|macos-bundled) echo boot-piano-macos-ram-A-bundled.img ;;
    uefi-B-signed) echo boot-piano-uefi-ram-B-own303-signed.img ;;
    uefi-A-signed) echo boot-piano-uefi-ram-A-bundled-signed.img ;;
    macos-B-signed) echo boot-piano-macos-ram-B-own303-signed.img ;;
    macos-A-signed) echo boot-piano-macos-ram-A-bundled-signed.img ;;
    *) echo "" ;;
  esac
}

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
CANDIDATES=()
[ -n "${PIANO_RAM_DIR:-}" ] && CANDIDATES+=("$PIANO_RAM_DIR")
CANDIDATES+=(
  "$SCRIPT_DIR"
  "$SCRIPT_DIR/../out/ram"
  "$HOME/Downloads/piano-ram-boot-ready"
  "$HOME/Downloads/XiaomiPad8Pro-Ubuntu/UniXNU-piano/out/ram"
  ${PIANO_BACKUP_DIR:+"$PIANO_BACKUP_DIR/ram-boot-ready"}
)

RAM_DIR=""
for d in "${CANDIDATES[@]}"; do
  [ -n "$d" ] || continue
  if [ -d "$d" ] && [ -f "$d/boot-piano-uefi-ram-B-own303.img" ]; then
    RAM_DIR=$(CDPATH= cd -- "$d" && pwd)
    break
  fi
done
[ -n "$RAM_DIR" ] || die "找不到 RAM 镜像目录（需要其中有 boot-piano-uefi-ram-B-own303.img）。可设置 PIANO_RAM_DIR。"

SUMS="$RAM_DIR/SHA256SUMS"
[ -f "$SUMS" ] || die "缺少哈希清单: $SUMS"

if [ -n "$IMAGE_ARG" ]; then
  mapped=$(alias_to_name "$IMAGE_ARG" || true)
  if [ -n "$mapped" ]; then
    IMAGE_NAME="$mapped"
  elif [ -f "$IMAGE_ARG" ]; then
    IMAGE_PATH=$(CDPATH= cd -- "$(dirname "$IMAGE_ARG")" && pwd)/$(basename "$IMAGE_ARG")
    IMAGE_NAME=$(basename "$IMAGE_PATH")
    # If the file lives outside RAM_DIR, still allow it but require a matching SUMS hash if present.
    if [ ! -f "$RAM_DIR/$IMAGE_NAME" ]; then
      RAM_DIR=$(dirname "$IMAGE_PATH")
      SUMS="$RAM_DIR/SHA256SUMS"
    fi
  elif [ -f "$RAM_DIR/$IMAGE_ARG" ]; then
    IMAGE_NAME="$IMAGE_ARG"
  else
    die "无法解析 --image $IMAGE_ARG"
  fi
else
  IMAGE_NAME=$(alias_to_name "$IMAGE_KEY")
fi
IMAGE_PATH="$RAM_DIR/$IMAGE_NAME"
[ -f "$IMAGE_PATH" ] || die "镜像不存在: $IMAGE_PATH"

expected=$(awk -v n="$IMAGE_NAME" '$2==n || $NF==n {print $1; found=1} END{exit !found}' "$SUMS" 2>/dev/null || true)
[ -n "$expected" ] || die "SHA256SUMS 中没有 $IMAGE_NAME"

if [ "$MODE" = list ]; then
  echo "镜像目录: $RAM_DIR"
  echo "哈希清单: $SUMS"
  echo "建议顺序:"
  echo "  1. boot-piano-uefi-ram-B-own303.img          （首选 M1）"
  echo "  2. boot-piano-uefi-ram-A-bundled.img         （没画面时）"
  echo "  3. boot-piano-macos-ram-B-own303.img         （之后 M2，菜单选 1 CPU）"
  echo
  echo "当前将使用: $IMAGE_PATH"
  echo "只进内存、不写分区。默认不加 --boot 不会启动设备。"
  exit 0
fi

log "主机: $(hostname -s 2>/dev/null || hostname)"
log "镜像目录: $RAM_DIR"
log "目标镜像: $IMAGE_PATH"
log "期望 SHA-256: $expected"

got=$(sha256_of "$IMAGE_PATH")
if [ "$got" != "$expected" ]; then
  die "哈希不匹配: $IMAGE_NAME
  期望 $expected
  实际 $got"
fi
bytes=$(wc -c < "$IMAGE_PATH" | tr -d ' ')
log "哈希一致 ($bytes 字节)"

# Optionally check every file listed in SHA256SUMS that is present (still offline, no device).
if [ "${PIANO_CHECK_ALL:-1}" = 1 ]; then
  bad=0
  good=0
  missing=0
  while read -r hash name; do
    [ -n "${hash:-}" ] || continue
    case "$hash" in \#*) continue ;; esac
    name=${name##*/}
    f="$RAM_DIR/$name"
    if [ ! -f "$f" ]; then
      missing=$((missing+1))
      log "缺文件: $name"
      continue
    fi
    g=$(sha256_of "$f")
    if [ "$g" = "$hash" ]; then
      good=$((good+1))
    else
      bad=$((bad+1))
      log "哈希失败: $name"
    fi
  done < "$SUMS"
  log "目录内哈希核对: ${good} 通过, ${bad} 失败, ${missing} 缺失"
  [ "$bad" -eq 0 ] || die "有镜像哈希失败，中止"
fi

FB=$(resolve_fastboot) || die "找不到 fastboot。请确认 platform-tools 在 PATH 中（建议 37.0.1）。"
log "fastboot: $FB"
log "$($FB --version 2>&1 | head -n 1)"

if command -v adb >/dev/null 2>&1; then
  log "adb: $(command -v adb) ($($(command -v adb) version 2>&1 | head -n 1))"
else
  log "adb: 未在 PATH 中（RAM 启动不需要 adb）"
fi

# Parse `fastboot devices` — ignore empty / leftover header lines.
DEV_OUT=$($FB devices 2>&1 || true)
DEV_LINES=$(printf '%s\n' "$DEV_OUT" | awk 'NF>=2 && $2 ~ /fastboot|device|recovery/ {print}' || true)
DEV_COUNT=$(printf '%s\n' "$DEV_LINES" | awk 'NF{c++} END{print c+0}')
if [ "$DEV_COUNT" -gt 0 ]; then
  log "fastboot 可见设备:"
  printf '%s\n' "$DEV_LINES"
else
  log "当前没有可见的 fastboot 设备。"
  log "（进 Fastboot：长按电源 + 音量下；系统运行中也可用 adb reboot bootloader / sudo piano-reboot-bootloader）"
  if [ -n "$DEV_OUT" ]; then
    log "fastboot devices 原始输出:"
    printf '%s\n' "$DEV_OUT"
  fi
fi

if [ "$MODE" = check ]; then
  log
  log "检查完成。未对设备执行 boot 或 flash。"
  log "准备好且 verify.sh 通过后，在现场执行:"
  log "  $0 --boot"
  log "完整等价命令:"
  log "  $FB boot $IMAGE_PATH"
  if [ "$DEV_COUNT" -eq 0 ]; then
    exit 0
  fi
  exit 0
fi

# MODE=boot
if [ "$DEV_COUNT" -eq 0 ]; then
  die "未看到 fastboot 设备，拒绝执行 boot。请插上 USB-C 数据线并进入 Fastboot 后再加 --boot。"
fi
if [ "$DEV_COUNT" -gt 1 ]; then
  die "看到多台 fastboot 设备，拒绝执行，以免 boot 到错误设备。请只留平板连接，或设置 ANDROID_SERIAL。"
fi

STAMP=$(date +%Y%m%d-%H%M%S)
if [ -n "${PIANO_BOOT_LOGDIR:-}" ]; then
  LOGDIR="$PIANO_BOOT_LOGDIR"
else
  LOGDIR="$HOME/Downloads/piano-ram-boot-ready/logs/home-ram-boot-$STAMP"
fi
mkdir -p "$LOGDIR"
LOG="$LOGDIR/fastboot-boot.log"

log
log "即将执行（只进内存、不写分区）:"
log "  $FB boot $IMAGE_PATH"
log "日志: $LOG"
{
  echo "# $STAMP $(hostname)"
  echo "# $FB boot $IMAGE_PATH"
  echo "# sha256 $got"
  echo "# this is RAM boot only; no flash"
} > "$LOG"

# Extra safety: never pass 'flash' to fastboot. Only 'boot' plus the image path.
set +e
"$FB" boot "$IMAGE_PATH" 2>&1 | tee -a "$LOG"
rc=${PIPESTATUS[0]:-${pipestatus[1]:-$?}}
set -e
echo "# exit $rc" >> "$LOG"
if [ "$rc" -ne 0 ]; then
  die "fastboot boot 退出码 $rc。设备分区不应被写入。可用电源+音量下回到 Fastboot，再 fastboot reboot 回 Ubuntu。日志: $LOG"
fi
log "fastboot boot 已返回 0。若无画面：长按电源重启，再试 uefi-A（$0 --boot --image uefi-A）。"
log "未写任何分区。"
exit 0
