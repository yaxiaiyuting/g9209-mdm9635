#!/system/bin/sh
# EXP12：★ 从"上电即刻"就把 AP2MDM_HSIC_READY 拉高，并**全程保持**
#
# 动机（两条硬证据）：
#  1) modem 自己的 crash log：/cpdump/mdm_err.log =
#     "There is not valid Crash Reason, so I guess this crash happen before
#      running the err_init() on apps_proc !"
#     → AMSS 死在非常早的阶段（错误处理器都还没初始化）
#  2) MDM_ERR_FATAL.BIN 全零 + MDM2AP_ERRFATAL 全程 lo + RST_STAT=2 / PmicPONstat 有置位
#     → 不是软件 fault，是被复位/看门狗
#
# modem 固件里 `- Ignore because Host ready GPIO is Low` 说明它是**读电平**。
# 之前我们都在"STATUS 拉高之后"才拉高 HSIC_READY，甚至 EXP11 也只提前到 Sahara 之前。
# 本实验把它提前到**上电瞬间**并全程保持，看 AMSS 是否就能走完初始化并枚举。
#
# 用法：EXP12 <mode>
#   hold : 上电即刻拉高并全程保持（默认，新假设）
#   none : 完全不动 HSIC_READY（对照）
D=/data/local/tmp/diag; M=/data/local/tmp/mdm; LOG=$D/exp12.log
MODE=${1:-hold}
echo "===== EXP12 START $(date) uptime=$(cut -d. -f1 /proc/uptime) mode=${MODE} =====" > $LOG

mount -o remount,rw / 2>/dev/null
printf '#!/system/bin/sh\nexit 0\n' > /system/bin/ks; chmod 755 /system/bin/ks
mkdir -p /firmware
mount -t vfat -o ro,shortname=lower,fmask=0133,dmask=0022 /dev/block/platform/15570000.ufs/by-name/RADIO /firmware 2>/dev/null
mkdir -p /dev/block/modem
for p in m9kefs1 m9kefs2 m9kefs3; do ln -sf /dev/block/platform/15570000.ufs/by-name/$p /dev/block/modem/$p; done
setprop ctl.stop ril-daemon 2>/dev/null; setprop ctl.stop ril-daemon1 2>/dev/null; setprop ctl.stop cpboot-daemon 2>/dev/null
sleep 2
for n in mdm_runtime_handshake esoc_reqeng esoc_pwron_hold ldrx mdm_helper ks.real ks_transfer powerup; do pkill -9 -x "$n" 2>/dev/null; done
sleep 1

st(){ cat /sys/kernel/debug/gpio 2>/dev/null | grep -E "$1" | awk '{print $NF}'; }
usbdevs(){ U=""; for d in /sys/bus/usb/devices/*/; do v=$(cat $d/idVendor 2>/dev/null); case "$v" in ""|1d6b) ;; *) U="$U $v:$(cat $d/idProduct 2>/dev/null)";; esac; done; [ -z "$U" ] && U=" -"; echo "$U"; }

# 1) 常驻请求引擎
nohup setsid $M/esoc_reqeng >/dev/null 2>&1 </dev/null &
sleep 3
echo "[1] reqeng=$(pgrep -x esoc_reqeng) hsic=$(st AP2MDM_HSIC_READY)" >> $LOG

# 2) 上电（常驻持 fd）——注意：内核 ESOC_PWR_ON 会把 HSIC_READY 清 0
nohup setsid $M/esoc_pwron_hold >/dev/null 2>&1 </dev/null &
sleep 2

# 3) ★ 上电后立刻拉高 HSIC_READY，并起一个守护每 200ms 重新拉高（防止被清）
if [ "$MODE" = "hold" ]; then
  $M/esoc_hsic >> $LOG 2>&1
  echo ">>> $(date +%H:%M:%S.%N|cut -c1-12) [hold] 上电后立刻拉高 HSIC_READY，hsic=$(st AP2MDM_HSIC_READY)" >> $LOG
  # 后台守护：持续保持 hi
  cat > /data/local/tmp/keep_hsic.sh <<'KEEP'
#!/system/bin/sh
M=/data/local/tmp/mdm
for i in $(seq 1 900); do
  V=$(cat /sys/kernel/debug/gpio 2>/dev/null | grep AP2MDM_HSIC_READY | awk '{print $NF}')
  [ "$V" = "lo" ] && $M/esoc_hsic >/dev/null 2>&1
  sleep 0.2
done
KEEP
  chmod 755 /data/local/tmp/keep_hsic.sh
  nohup setsid /system/bin/sh /data/local/tmp/keep_hsic.sh >/dev/null 2>&1 </dev/null &
fi
sleep 6

# 4) 加载器喂 12 镜像
rm -f $D/sahara_own.log
nohup setsid $M/ldrx >/dev/null 2>&1 </dev/null &
sleep 2
echo "[2] ldrx=$(pgrep -x ldrx) hsic=$(st AP2MDM_HSIC_READY)" >> $LOG

# 5) 全程高频采样（含 HSIC 电平 / USB 设备 / efs 桥）
i=0
while [ $i -lt 900 ]; do
  i=$((i+1))
  EFS=$( [ -e /dev/efs_hsic_bridge ] && echo EFS || echo -)
  ENDS=$(grep -c "END_IMAGE_TX" $D/sahara_own.log 2>/dev/null || echo 0)
  echo "$(date +%H:%M:%S.%N|cut -c1-12) ends=$ENDS ST=$(st MDM2AP_STATUS) H=$(st AP2MDM_HSIC_READY) PBL=$(st MDM2AP_PBLRDY) ERR=$(st MDM2AP_ERRFATAL) efs=$EFS usb=[$(usbdevs)]" >> $LOG
  sleep 0.2
done
echo "===== EXP12 END $(date) =====" >> $LOG
echo "DONE-MARKER"
