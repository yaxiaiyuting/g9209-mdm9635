#!/bin/bash
# restore-via-download-mode.sh — 手机在 Download 模式时，用 heimdall 恢复到 LOS 20
#
# 前提：手机已进 Download 模式（关机 → 音量下 + Home + 电源 → 警告页按音量上）
#       heimdall udev 规则已装（/etc/udev/rules.d/60-heimdall.rules）
#
# 用法: sudo bash restore-via-download-mode.sh [--full]
set -u
BK=/home/duanjb666/deepseek/G9209-fix/backup-20261003-full
FULL=0; [ "${1:-}" = "--full" ] && FULL=1

echo "═══ heimdall 恢复 LOS 20  $(date '+%F %T') ═══"

if ! heimdall detect 2>&1 | grep -qi "detected"; then
  echo "!! 未检测到 Download 模式设备。"
  echo "   请：关机 → 按住 音量下 + Home + 电源 → 出现警告页后按 音量上"
  echo "   然后重新运行本脚本。"
  exit 1
fi
echo "★ 检测到 Download 模式设备"
echo

# 现有 PIT（重要：分区名要对上）
PIT=/tmp/device.pit
echo "--- 下载设备 PIT ---"
heimdall download-pit --output "$PIT" --no-reboot 2>&1 | tail -3
if [ -f "$PIT" ]; then
  echo "PIT 分区表（前 40 行）："
  heimdall print-pit --file "$PIT" 2>/dev/null | grep -E "Partition Name|Flash Filename" | head -40
fi
echo

ARGS="--no-reboot"
for p in BOOT SYSTEM RECOVERY; do
  IMG="$BK/$p.img"
  [ -f "$IMG" ] || { echo "!! 缺 $IMG"; continue; }
  echo "  计划: --$p $IMG  ($(stat -c %s "$IMG") 字节)"
  ARGS="$ARGS --$p $IMG"
done
if [ $FULL = 1 ]; then
  for p in EFS RADIO PARAM PERSDATA; do
    IMG="$BK/$p.img"
    [ -f "$IMG" ] && { echo "  计划: --$p $IMG"; ARGS="$ARGS --$p $IMG"; }
  done
  echo "  ⚠️ m9kefs1/2/3 在 heimdall 里可能叫别的名字，请先看上面的 PIT 分区名再手动加"
fi
echo
echo "--- 开始刷写 ---"
# shellcheck disable=SC2086
heimdall flash $ARGS 2>&1 | tail -25
RC=$?
echo
if [ $RC = 0 ]; then
  echo "✅ heimdall 刷写完成。设备将重启到 LOS 20。"
else
  echo "⚠️ heimdall 返回 $RC —— 请检查上面输出。"
  echo "   备注：若提示分区名不存在，用 'heimdall print-pit' 的分区名替换。"
fi
exit $RC
