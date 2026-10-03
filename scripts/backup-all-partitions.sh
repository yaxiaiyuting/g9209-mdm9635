#!/bin/bash
# 全机分区完整备份（raw dump 到主机）
# 用法: bash backup-all-partitions.sh [输出目录]
set -u
OUT="${1:-/home/duanjb666/deepseek/G9209-fix/backup-20261003-full}"
BYNAME=/dev/block/platform/15570000.ufs/by-name
mkdir -p "$OUT"
MAN="$OUT/MANIFEST.txt"

echo "=== 全机备份开始 $(date '+%F %T') ===" | tee "$MAN"
echo "输出目录: $OUT" | tee -a "$MAN"
echo "" | tee -a "$MAN"

# 列出所有分区
PARTS=$(adb shell "ls $BYNAME" | tr -d '\r' | sort)
echo "发现分区: $(echo $PARTS | tr '\n' ' ')" | tee -a "$MAN"
echo "" | tee -a "$MAN"

FAIL=0
for p in $PARTS; do
  [ -z "$p" ] && continue
  SIZE=$(adb shell "blockdev --getsize64 $BYNAME/$p" 2>/dev/null | tr -d '\r')
  [ -z "$SIZE" ] && { echo "!! $p 取不到大小，跳过" | tee -a "$MAN"; FAIL=1; continue; }

  # 优先用设备端已有的备份（避免重复读盘）；否则 dump
  REMOTE="/data/local/tmp/diag/boot-before-AE.img"
  LOCAL="$OUT/$p.img"

  if [ -f "$LOCAL" ] && [ "$(stat -c %s "$LOCAL")" = "$SIZE" ]; then
    echo "$p: 已存在，跳过 dump" | tee -a "$MAN"
  elif [ "$SIZE" -gt 4000000000 ]; then
    # ★ 大分区（>4GB，实际就是 USERDATA）必须**直连流式**拉取：
    #   曾经用"先 dd 到设备端 /data/local/tmp 再 pull"的做法，
    #   而 /data 空闲(21GB) 小于 USERDATA(28GB) → 写到 22.7GB 时空间耗尽，
    #   拿到的镜像不完整（FAIL=1）。改用 adb exec-out 直接落到主机即可。
    echo "--- $p ($SIZE 字节) 直连流式拉取（不经设备端暂存）---" | tee -a "$MAN"
    adb exec-out "dd if=$BYNAME/$p bs=1048576 2>/dev/null" > "$LOCAL"
  else
    echo "--- $p ($SIZE 字节) ---" | tee -a "$MAN"
    adb shell "dd if=$BYNAME/$p of=/data/local/tmp/bk_$p.img bs=1048576 2>&1 | tail -1" | tee -a "$MAN"
    adb pull "/data/local/tmp/bk_$p.img" "$LOCAL" 2>&1 | tail -1 | tee -a "$MAN"
    adb shell "rm -f /data/local/tmp/bk_$p.img"
  fi

  ACTUAL=$(stat -c %s "$LOCAL" 2>/dev/null || echo 0)
  if [ "$ACTUAL" != "$SIZE" ]; then
    echo "!! $p 大小不符: 期望 $SIZE 实际 $ACTUAL" | tee -a "$MAN"; FAIL=1
  fi
  M=$(md5sum "$LOCAL" 2>/dev/null | cut -d' ' -f1)
  echo "$p size=$ACTUAL md5=$M" | tee -a "$MAN"
done

echo "" | tee -a "$MAN"
echo "=== 汇总 ===" | tee -a "$MAN"
ls -la "$OUT" | tee -a "$MAN"
du -sh "$OUT" | tee -a "$MAN"
echo "FAIL=$FAIL" | tee -a "$MAN"
echo "=== 结束 $(date '+%F %T') ===" | tee -a "$MAN"
exit $FAIL
