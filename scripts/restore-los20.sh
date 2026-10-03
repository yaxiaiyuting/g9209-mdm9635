#!/bin/bash
# restore-los20.sh — 一键把手机恢复到 LOS 20（本次实验前状态）
#
# 支持的设备状态：
#   1) TWRP 里（adb 可用）      → 直接刷，最推荐
#   2) 已开机进系统（adb 可用）  → 直接刷
#   3) Download 模式            → 提示用 heimdall/Odin（脚本给出命令）
#
# 用法:
#   bash restore-los20.sh              # 默认刷 BOOT + SYSTEM + RECOVERY
#   bash restore-los20.sh --full        # 连 EFS/RADIO/m9kefs/PARAM 一起刷
#   bash restore-los20.sh --wait        # 等设备出现后自动开刷
#
set -u
BK=/home/duanjb666/deepseek/G9209-fix/backup-20261003-full
BYNAME=/dev/block/platform/15570000.ufs/by-name
WAIT=0; FULL=0
for a in "$@"; do
  [ "$a" = "--wait" ] && WAIT=1
  [ "$a" = "--full" ] && FULL=1
done

PARTS="BOOT SYSTEM RECOVERY"
[ $FULL = 1 ] && PARTS="$PARTS EFS RADIO m9kefs1 m9kefs2 m9kefs3 PARAM PERSDATA"

echo "════════ 恢复到 LOS 20  $(date '+%F %T') ════════"
echo "将刷写分区: $PARTS"
echo

# ---- 等设备 ----
if [ $WAIT = 1 ]; then
  echo "等待设备（adb，最多 15 分钟）..."
  for i in $(seq 1 180); do
    if adb devices 2>/dev/null | tail -n +2 | grep -qw device; then
      echo "★ 设备已连接（第 ${i} 次探测）"; break
    fi
    sleep 5
  done
fi

if ! adb devices 2>/dev/null | tail -n +2 | grep -qw device; then
  echo "!! 当前没有 adb 设备。请先让手机进入 TWRP 或正常开机。"
  echo
  echo "   进 TWRP:  关机后按住 音量上 + Home + 电源"
  echo "   Download: 关机后按住 音量下 + Home + 电源 → 警告页按 音量上"
  echo
  echo "   若已在 Download 模式，用 heimdall 刷（需 sudo 访问 USB）："
  echo "     sudo heimdall print-pit --no-reboot | head -40"
  echo "     sudo heimdall flash --no-reboot \\"
  echo "       --BOOT     $BK/BOOT.img \\"
  echo "       --SYSTEM   $BK/SYSTEM.img \\"
  echo "       --RECOVERY $BK/RECOVERY.img"
  exit 1
fi

MODE=$(adb shell 'getprop ro.twrp.version 2>/dev/null; echo X' 2>/dev/null | head -1 | tr -d '\r')
if [ -n "${MODE:-}" ] && [ "$MODE" != "X" ]; then
  echo "检测到 TWRP: $MODE"
else
  echo "检测到已开机系统"
fi
echo

FAIL=0
for p in $PARTS; do
  IMG="$BK/$p.img"
  [ -f "$IMG" ] || { echo "!! 缺少 $IMG，跳过"; FAIL=1; continue; }
  LOCAL=$(md5sum "$IMG" | cut -d' ' -f1)
  SIZE=$(stat -c %s "$IMG")
  echo "──── $p ($SIZE 字节, ${LOCAL:0:16}…) ────"

  if ! adb push "$IMG" "/data/local/tmp/rb_$p.img" 2>&1 | tail -1; then
    echo "   !! 推送失败"; FAIL=1; continue
  fi
  GOT=$(adb shell "md5sum /data/local/tmp/rb_$p.img 2>/dev/null" | cut -d' ' -f1 | tr -d '\r')
  if [ "$GOT" != "$LOCAL" ]; then
    echo "   !! 推送后 md5 不符（设备 $GOT）→ **跳过刷写**"; FAIL=1; continue
  fi
  echo "   推送校验 OK"

  adb shell "dd if=/data/local/tmp/rb_$p.img of=$BYNAME/$p bs=1M 2>&1 | tail -1"
  adb shell "sync"

  R=$(adb shell "md5sum $BYNAME/$p 2>/dev/null" | cut -d' ' -f1 | tr -d '\r')
  if [ "$R" = "$LOCAL" ]; then
    echo "   ✅ 刷写校验通过"
  else
    echo "   ⚠️ 回读 md5 不同: 设备 ${R:0:16}… / 期望 ${LOCAL:0:16}…"
    echo "      （若镜像小于分区，尾部保持原样 → md5 不同属正常）"
  fi
  adb shell "rm -f /data/local/tmp/rb_$p.img"
done

echo
echo "════════ 结束 FAIL=$FAIL ════════"
[ $FAIL = 0 ] && echo "重启到系统: adb reboot"
exit $FAIL
