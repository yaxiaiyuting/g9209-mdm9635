#!/system/bin/sh
# EXP13：★ 让原厂 mdm_helper 驱动整条链路（它自己调 /system/bin/ks）
#   /system/bin/ks 换成我们的转发器 ks_passthru（丢掉 -r ramdump 开关，转发给 ks.real）
#   mdm_helper 负责：注册 req engine、powerup、HSIC unbind/bind、调 ks、
#                    ESOC_IMG_XFER_DONE、等 STATUS、MODE_RUNTIME（unbind/bind + HSIC_READY）、
#                    等 /dev/efs_hsic_bridge、再调 ks 做 EFS 同步、ESOC_BOOT_DONE
# 我们只负责：环境 + 上电 + 记录
D=/data/local/tmp/diag; M=/data/local/tmp/mdm; LOG=$D/exp13.log
echo "===== EXP13 START $(date) uptime=$(cut -d. -f1 /proc/uptime) =====" > $LOG

mount -o remount,rw / 2>/dev/null
# ★ ks 换成转发器（原厂 mdm_helper 会调它）
cp -f $M/ks_passthru /system/bin/ks
chmod 755 /system/bin/ks
chcon u:object_r:system_file:s0 /system/bin/ks 2>/dev/null
echo "[env] ks=$(md5sum /system/bin/ks|cut -c1-8) $(ls -la /system/bin/ks|awk '{print $1}')" >> $LOG

mkdir -p /firmware
mount -t vfat -o ro,shortname=lower,fmask=0133,dmask=0022 /dev/block/platform/15570000.ufs/by-name/RADIO /firmware 2>/dev/null
mkdir -p /dev/block/modem
for p in m9kefs1 m9kefs2 m9kefs3; do ln -sf /dev/block/platform/15570000.ufs/by-name/$p /dev/block/modem/$p; done
chown system:radio /dev/block/modem /dev/block/modem/* 2>/dev/null
mkdir -p /cpdump; chmod 777 /cpdump 2>/dev/null
setprop ctl.stop ril-daemon 2>/dev/null; setprop ctl.stop ril-daemon1 2>/dev/null; setprop ctl.stop cpboot-daemon 2>/dev/null
sleep 2
# ★ 注意：不要杀 mdm_helper 之外的；但也别让我们自己的 reqeng 抢引擎
for n in mdm_runtime_handshake esoc_reqeng ldrx ks_transfer powerup; do pkill -9 -x "$n" 2>/dev/null; done
pkill -9 -x mdm_helper 2>/dev/null
sleep 1

st(){ cat /sys/kernel/debug/gpio 2>/dev/null | grep -E "$1" | awk '{print $NF}'; }
usbdevs(){ U=""; for d in /sys/bus/usb/devices/*/; do v=$(cat $d/idVendor 2>/dev/null); case "$v" in ""|1d6b) ;; *) U="$U $v:$(cat $d/idProduct 2>/dev/null)";; esac; done; [ -z "$U" ] && U=" -"; echo "$U"; }

# 1) 起 mdm_helper（它注册 req engine；然后等 PBLRDY）
export LD_LIBRARY_PATH=/system/lib64
nohup setsid $M/mdm_helper >$D/mh13.log 2>&1 </dev/null &
sleep 5
echo "[1] mdm_helper=$(pgrep -x mdm_helper)" >> $LOG

# 2) 上电（mdm_helper 会自己等 boot_done；这里用普通 powerup 即可，
#    因为 helper 持有 fd 直到它自己结束）
nohup setsid $M/powerup /dev/subsys_esoc0 >/dev/null 2>&1 </dev/null &
sleep 6
echo "[2] 上电后 $(date +%H:%M:%S) gpio: AP2MDM_STATUS=$(st AP2MDM_STATUS) PBLRDY=$(st MDM2AP_PBLRDY) hsic=$(st AP2MDM_HSIC_READY)" >> $LOG

# 3) 高频记录 12 分钟（覆盖 mdm_helper 的 61s STATUS 等待 + 37.5s efs 等待）
i=0
while [ $i -lt 720 ]; do
  i=$((i+1))
  EFS=$( [ -e /dev/efs_hsic_bridge ] && echo EFS || echo -)
  echo "$(date +%H:%M:%S) ST=$(st MDM2AP_STATUS) H=$(st AP2MDM_HSIC_READY) PBL=$(st MDM2AP_PBLRDY) ERR=$(st MDM2AP_ERRFATAL) efs=$EFS helper=$(pgrep -c -x mdm_helper) usb=[$(usbdevs)]" >> $LOG
  sleep 1
done
echo "===== EXP13 END $(date) =====" >> $LOG
echo "--- mdm_helper log ---" >> $LOG; cat $D/mh13.log >> $LOG 2>&1
echo "--- kspassthru log ---" >> $LOG; cat $D/kspassthru.log >> $LOG 2>&1
echo "DONE-MARKER"
