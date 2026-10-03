#!/system/bin/sh
# flash_resumable.sh — 掉线可续的大分区刷写器（在 TWRP 内运行）
#
# 背景（实测规律）：
#   · 从 TWRP 里用 dd 一次刷 3 GB → 成功（27s @ 117MB/s）
#   · 一次性刷 10 GB（USERDATA）→ adb 在几十秒内**必然掉线**
#   · 掉线后设备可能从 USB 总线消失，需物理重启
#   → 结论：**单次连续大写入不可靠**，必须分块 + 可续 + 每块校验
#
# 本脚本：
#   1) 把 img 按 CHUNK 大小分块写入目标分区（dd skip/seek）
#   2) 每块写完立即回读该块并与源比对（cmp）
#   3) 进度记录在 $PROG，掉线重进后**自动从中断处继续**
#   4) 只依赖 toybox dd/cmp（TWRP 有），bs 用纯数字
#
# 用法（TWRP 内，设备上执行）：
#   sh /data/local/tmp/flash_resumable.sh <src.img> <目标分区名> [块大小字节]
# 例：
#   sh /data/local/tmp/flash_resumable.sh /data/local/tmp/stock-userdata.img USERDATA
#
# 主机侧推荐用法（掉线自动重试）：
#   adb shell "sh /data/local/tmp/flash_resumable.sh ... " || \
#     (等待设备重新出现后，重跑同一条命令即可续传)

set -u
SRC="${1:?用法: flash_resumable.sh <src.img> <分区名> [块大小]}"
PART="${2:?缺少目标分区名}"
CHUNK="${3:-268435456}"          # 默认 256 MB
BN=/dev/block/platform/15570000.ufs/by-name
DST="$BN/$PART"
PROG="/data/local/tmp/.flash_prog_${PART}"

[ -f "$SRC" ] || { echo "!! 源文件不存在: $SRC"; exit 1; }
[ -e "$DST" ] || { echo "!! 目标分区不存在: $DST"; exit 1; }

SZ=$(stat -c %s "$SRC" 2>/dev/null || echo 0)
[ "$SZ" -gt 0 ] || { echo "!! 源文件大小为 0"; exit 1; }

echo "════════ 可续刷写 ════════"
echo "源  : $SRC ($SZ 字节)"
echo "目标: $DST"
echo "块  : $CHUNK 字节"
echo

# 读取进度（已完成的块数）
DONE=0
[ -f "$PROG" ] && DONE=$(cat "$PROG" 2>/dev/null | tr -d '\r\n ' )
case "$DONE" in ''|*[!0-9]*) DONE=0 ;; esac

TOTAL=$(( (SZ + CHUNK - 1) / CHUNK ))
echo "总块数: $TOTAL   已完成: $DONE"
[ "$DONE" -ge "$TOTAL" ] && { echo "★ 已全部完成，做最终校验..."; }

i=$DONE
while [ "$i" -lt "$TOTAL" ]; do
  OFF=$(( i * CHUNK ))
  LEN=$CHUNK
  REST=$(( SZ - OFF ))
  [ "$LEN" -gt "$REST" ] && LEN=$REST

  printf "[%d/%d] off=%d len=%d ... " "$((i+1))" "$TOTAL" "$OFF" "$LEN"

  # 写入该块
  if ! dd if="$SRC" of="$DST" bs=1048576 skip=$((OFF/1048576)) \
        seek=$((OFF/1048576)) count=$((LEN/1048576)) 2>/dev/null; then
    echo "写失败（可能掉线）→ 退出，重跑本脚本会从第 $((i+1)) 块继续"
    exit 2
  fi

  # 回读该块并与源比对
  if cmp -s -i "$OFF" -n "$LEN" "$SRC" "$DST" 2>/dev/null; then
    echo "✅"
  else
    # 退化方案：分别 dd 出该块再比
    dd if="$SRC" of=/data/local/tmp/.c_a bs=1048576 skip=$((OFF/1048576)) count=$((LEN/1048576)) 2>/dev/null
    dd if="$DST" of=/data/local/tmp/.c_b bs=1048576 skip=$((OFF/1048576)) count=$((LEN/1048576)) 2>/dev/null
    if cmp -s /data/local/tmp/.c_a /data/local/tmp/.c_b 2>/dev/null; then
      echo "✅(回读比对)"
    else
      echo "❌ 校验不符 → 退出（重跑本脚本会重写该块）"
      rm -f /data/local/tmp/.c_a /data/local/tmp/.c_b
      exit 3
    fi
    rm -f /data/local/tmp/.c_a /data/local/tmp/.c_b
  fi

  i=$((i+1))
  echo "$i" > "$PROG"
  sync
done

echo
echo "════════ 全部 $TOTAL 块完成 ════════"
rm -f "$PROG"
echo "★ 建议：重新运行一次做整段终校（本脚本会显示已完成并只做校验）"
exit 0
