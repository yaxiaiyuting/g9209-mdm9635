#!/bin/bash
# flash-resilient.sh — 主机侧：推送镜像并"掉线可续"地刷写某个分区
#
# 解决的问题：从 TWRP 里刷大分区（>3GB）时 adb 会掉线、设备可能离总线。
# 本脚本把工作交给设备端的 flash_resumable.sh（分块 + 每块校验 + 进度落盘），
# 主机侧只负责：等设备回来 → 重跑同一条命令 → 直到报"全部完成"。
#
# 用法:
#   bash flash-resilient.sh <本地img> <分区名> [--push-only] [--chunk 字节]
# 例:
#   bash flash-resilient.sh stock-extract/userdata-raw.img USERDATA
#   bash flash-resilient.sh backup-20261003-full/SYSTEM.img SYSTEM --chunk 536870912
#
set -u
IMG="${1:?用法: flash-resilient.sh <本地img> <分区名> [--push-only] [--chunk N]}"
PART="${2:?缺少目标分区名}"
shift 2
PUSH_ONLY=0; CHUNK=268435456
while [ $# -gt 0 ]; do
  case "$1" in
    --push-only) PUSH_ONLY=1 ;;
    --chunk) shift; CHUNK="${1:-268435456}" ;;
    *) ;;
  esac
  shift
done

[ -f "$IMG" ] || { echo "!! 找不到 $IMG"; exit 1; }
SIZE=$(stat -c %s "$IMG")
LOCAL_MD5=$(md5sum "$IMG" | cut -d' ' -f1)
REMOTE="/data/local/tmp/fr_${PART}.img"
SCRIPT=/data/local/tmp/flash_resumable.sh

echo "════════ 掉线可续刷写 ════════"
echo "镜像  : $IMG"
echo "大小  : $SIZE 字节 ($(awk -v s=$SIZE 'BEGIN{printf "%.2f", s/1073741824}') GB)"
echo "md5   : $LOCAL_MD5"
echo "分区  : $PART"
echo "块大小: $CHUNK"
echo

wait_dev() {
  local n=0
  while [ $n -lt "${1:-120}" ]; do
    if adb devices 2>/dev/null | tail -n +2 | grep -qw device; then return 0; fi
    sleep 5; n=$((n+1))
  done
  return 1
}

echo "--- 等待设备（最多 10 分钟）---"
if ! wait_dev 120; then
  echo "!! 设备未出现。请：长按电源 10-20 秒断电 → 音量上+Home+电源 进 TWRP"
  exit 1
fi
echo "★ 设备已连接"

# --- 1) 推送设备端脚本 ---
adb push "$(dirname "$0")/flash_resumable.sh" "$SCRIPT" 2>&1 | tail -1
adb shell "chmod 755 $SCRIPT"

# --- 2) 推送镜像（若设备端已有且大小一致则跳过）---
RSIZE=$(adb shell "stat -c %s $REMOTE 2>/dev/null" | tr -d '\r')
if [ "$RSIZE" = "$SIZE" ]; then
  echo "--- 设备端已有同尺寸镜像，跳过推送 ---"
else
  echo "--- 推送镜像（大文件，可能较慢/掉线）---"
  if ! adb push "$IMG" "$REMOTE" 2>&1 | tail -1; then
    echo "!! 推送中断。重跑本脚本会自动续（或用 --push-only 反复重试）"
    exit 2
  fi
fi

# --- 3) 校验设备端镜像 ---
echo "--- 校验设备端镜像 ---"
RMD5=$(adb shell "md5sum $REMOTE 2>/dev/null" | cut -d' ' -f1 | tr -d '\r')
if [ "$RMD5" != "$LOCAL_MD5" ]; then
  echo "!! 设备端 md5 不符（$RMD5）→ 删除重推"
  adb shell "rm -f $REMOTE"
  exit 3
fi
echo "★ 镜像校验一致"

[ "$PUSH_ONLY" = 1 ] && { echo "（--push-only，结束）"; exit 0; }

# --- 4) 掉线可续地刷写 ---
echo
echo "--- 开始分块刷写（掉线会自动重试）---"
ATTEMPT=0
while [ $ATTEMPT -lt 30 ]; do
  ATTEMPT=$((ATTEMPT+1))
  OUT=$(adb shell "sh $SCRIPT $REMOTE $PART $CHUNK" 2>&1)
  RC=$?
  echo "$OUT" | tail -6

  if echo "$OUT" | grep -q "全部.*块完成"; then
    echo "✅ 第 $ATTEMPT 次尝试：全部完成"
    break
  fi

  echo "⚠️ 第 $ATTEMPT 次尝试中断（rc=$RC）——等待设备回来后续传..."
  if ! wait_dev 60; then
    echo "!! 设备长时间未回来。请物理重启进 TWRP，然后重跑本脚本（会从中断块继续）"
    exit 4
  fi
  echo "★ 设备已回来，继续..."
done

[ $ATTEMPT -ge 30 ] && { echo "!! 重试 30 次仍未完成"; exit 5; }

echo
echo "════════ 完成 ════════"
echo "建议在 TWRP 内做一次整段核对："
echo "  adb shell 'md5sum /dev/block/platform/15570000.ufs/by-name/$PART'"
echo "  （注意：分区大于镜像时，整段 md5 会与镜像不同，属正常；用上面脚本的逐块校验为准）"
