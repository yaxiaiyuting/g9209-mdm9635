#!/system/bin/sh
# EXP11：把 HSIC_READY 提前到 "Sahara 之前"（让 AMSS 起来时它已经是 hi）
#   并保留后续的 unbind/bind + 再脉冲。
#   动机：modem 固件里有 "- Ignore because Host ready GPIO is Low"，
#   说明它是**读电平**而非只认边沿。若 AMSS 起来时才第一次看到 hi，
#   某些内部状态机可能已经走过那一步（被 Ignore 掉），之后不会再回来。
# 用法：EXP11 <mode>
#   mode=early  : Sahara 前就拉高（本脚本默认）
#   mode=late   : 只在 STATUS=hi 之后拉高（= 之前 EXP8 的做法，做对照）
D=/data/local/tmp/diag; M=/data/local/tmp/mdm; LOG=$D/exp11.log
MODE=${1:-early}
echo "===== EXP11 START $(date) uptime=$(cut -d. -f1 /proc/uptime) mode=${MODE} =====" > $LOG

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

nohup setsid $M/esoc_reqeng >/dev/null 2>&1 </dev/null &
sleep 3
nohup setsid $M/esoc_pwron_hold >/dev/null 2>&1 </dev/null &
sleep 8

if [ "$MODE" = "early" ]; then
  $M/esoc_hsic >> $LOG 2>&1
  echo ">>> $(date +%H:%M:%S.%N|cut -c1-12) [early] HSIC_READY 已拉高 hsic=$(st AP2MDM_HSIC_READY)" >> $LOG
fi

rm -f $D/sahara_own.log
nohup setsid $M/ldrx >/dev/null 2>&1 </dev/null &
sleep 2
echo "[setup] eng=$(pgrep -x esoc_reqeng) hold=$(pgrep -x esoc_pwron_hold) ldrx=$(pgrep -x ldrx)" >> $LOG

usbdevs(){ U=""; for d in /sys/bus/usb/devices/*/; do v=$(cat $d/idVendor 2>/dev/null); case "$v" in ""|1d6b) ;; *) U="$U $v:$(cat $d/idProduct 2>/dev/null)";; esac; done; [ -z "$U" ] && U=" -"; echo "$U"; }

# 等 STATUS=hi
i=0; while [ $i -lt 3000 ]; do i=$((i+1)); [ "$(st MDM2AP_STATUS)" = "hi" ] && break; sleep 0.1; done
if [ "$(st MDM2AP_STATUS)" != "hi" ]; then
  echo "!! STATUS 未拉高（ends=$(grep -c END_IMAGE_TX $D/sahara_own.log 2>/dev/null)）" >> $LOG; exit 1
fi
echo ">>> $(date +%H:%M:%S.%N|cut -c1-12) ★ STATUS=hi hsic=$(st AP2MDM_HSIC_READY) usb=[$(usbdevs)]" >> $LOG

# EHCI unbind → 10ms → bind
echo 15510000.usb > /sys/bus/platform/drivers/s5p-ehci/unbind 2>>$LOG
sleep 0.01
echo 15510000.usb > /sys/bus/platform/drivers/s5p-ehci/bind 2>>$LOG
echo ">>> $(date +%H:%M:%S.%N|cut -c1-12) EHCI 已 bind" >> $LOG

if [ "$MODE" = "late" ]; then
  $M/esoc_hsic >> $LOG 2>&1
  echo ">>> $(date +%H:%M:%S.%N|cut -c1-12) [late] HSIC_READY 脉冲 hsic=$(st AP2MDM_HSIC_READY)" >> $LOG
fi

# 高频观察 30s
j=0
while [ $j -lt 300 ]; do
  j=$((j+1))
  EFS=$( [ -e /dev/efs_hsic_bridge ] && echo EFS || echo -)
  echo "$(date +%H:%M:%S.%N|cut -c1-12) ST=$(st MDM2AP_STATUS) H=$(st AP2MDM_HSIC_READY) PBL=$(st MDM2AP_PBLRDY) ERR=$(st MDM2AP_ERRFATAL) efs=$EFS usb=[$(usbdevs)]" >> $LOG
  sleep 0.1
done
echo "===== EXP11 END $(date) =====" >> $LOG
echo "DONE-MARKER"
