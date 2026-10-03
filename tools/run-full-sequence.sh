#!/system/bin/sh
# EXP8：完整复刻原厂时序
#   reqeng(常驻) + poweron-hold(常驻) + ldrx 喂镜像
#   → 等 MDM2AP_STATUS=hi → EHCI unbind/bind → HSIC_READY 上升沿
#   → 等 efs_hsic_bridge → ESOC_BOOT_DONE → ESOC_DIAG_DISABLE
# ★ 与之前的关键差异：**不再在 Sahara 之前调用 esoc_hsic**（HSIC_READY 必须晚于 STATUS）
D=/data/local/tmp/diag; M=/data/local/tmp/mdm; LOG=$D/exp8.log
echo "===== EXP8 START $(date) uptime=$(cut -d. -f1 /proc/uptime) =====" > $LOG

mount -o remount,rw / 2>/dev/null
printf '#!/system/bin/sh\nexit 0\n' > /system/bin/ks
chmod 755 /system/bin/ks
mkdir -p /firmware
mount -t vfat -o ro,shortname=lower,fmask=0133,dmask=0022 /dev/block/platform/15570000.ufs/by-name/RADIO /firmware 2>/dev/null
mkdir -p /dev/block/modem
for p in m9kefs1 m9kefs2 m9kefs3; do ln -sf /dev/block/platform/15570000.ufs/by-name/$p /dev/block/modem/$p; done
setprop ctl.stop ril-daemon 2>/dev/null; setprop ctl.stop ril-daemon1 2>/dev/null
setprop ctl.stop cpboot-daemon 2>/dev/null
sleep 2
# 先停"要用 /dev/esoc-0 的"常驻工具，避免抢 req engine
for n in mdm_runtime_handshake esoc_reqeng esoc_pwron_hold ldrx mdm_helper ks.real ks_transfer powerup; do pkill -9 -x "$n" 2>/dev/null; done
sleep 1

# 1) 常驻请求引擎
nohup setsid $M/esoc_reqeng >/dev/null 2>&1 </dev/null &
sleep 3
# 2) 上电（常驻持 fd）
nohup setsid $M/esoc_pwron_hold >/dev/null 2>&1 </dev/null &
sleep 8
# 3) 加载器喂 12 镜像
rm -f $D/sahara_own.log
nohup setsid $M/ldrx >/dev/null 2>&1 </dev/null &
sleep 2
# 4) 运行时握手守护（它会自己等 STATUS=hi）
nohup setsid $M/mdm_runtime_handshake >/dev/null 2>&1 </dev/null &
sleep 2
echo "[setup] eng=$(pgrep -x esoc_reqeng) hold=$(pgrep -x esoc_pwron_hold) ldrx=$(pgrep -x ldrx) rt=$(pgrep -x mdm_runtime_handshake)" >> $LOG

usbpids(){ for d in /sys/bus/usb/devices/*/; do v=$(cat $d/idVendor 2>/dev/null); [ "$v" = "05c6" ] && echo -n "$(cat $d/idProduct 2>/dev/null)/if$(cat $d/bInterfaceNumber 2>/dev/null) "; done; }
# 也列出所有 usb 设备的 product 名
usball(){ for d in /sys/bus/usb/devices/*/; do p=$(cat $d/product 2>/dev/null); [ -n "$p" ] && echo -n "$(cat $d/idVendor 2>/dev/null):$(cat $d/idProduct 2>/dev/null)=$p "; done; }

i=0
while [ $i -lt 220 ]; do
  i=$((i+1)); TS=$(date +%H:%M:%S)
  ST=$(cat /sys/kernel/debug/gpio 2>/dev/null | grep "MDM2AP_STATUS" | awk '{print $NF}')
  EFS=$( [ -e /dev/efs_hsic_bridge ] && echo EFS || echo -)
  DB=$( [ -e /dev/diag_bridge ] && echo DIAG || echo -)
  ENDS=$(grep -c "END_IMAGE_TX" $D/sahara_own.log 2>/dev/null || echo 0)
  SST=$(cat /sys/devices/qcom,mdm1.54/esoc0/subsys0/state 2>/dev/null)
  echo "$TS ends=$ENDS STATUS=$ST $EFS $DB subsys=$SST usb=[$(usbpids)] all=[$(usball)]" >> $LOG
  sleep 2
done
echo "===== EXP8 END $(date) =====" >> $LOG
echo "--- runtime.log ---" >> $LOG; cat $D/runtime.log >> $LOG 2>&1
echo "--- reqeng.log ---" >> $LOG; cat $D/reqeng.log >> $LOG 2>&1
echo "DONE-MARKER"
