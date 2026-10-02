# SM-G9209（Galaxy S6 电信版）外挂高通 MDM9635 基带 —— 逆向与可用性研究

> 目标：让 **SM-G9209**（`zerofltectc`，Exynos7420 + 外挂 Qualcomm MDM9635）在
> **LineageOS 20 (Android 13)** 上重新用上基带（读 SIM / 上网）。
> 本仓库是该研究的**完整技术文档、工具源码、内核补丁与可刷机包**。

---

## 现在能不能用？

**还不能读卡/上网。** 当前进度与卡点（诚实版）：

| 部分 | 状态 |
|---|---|
| AP / 内核（LOS 20 能正常启动、无 panic） | ✅ 完成 |
| Sahara over HSIC 协议逆向 | ✅ 完成（帧格式、全部命令、原厂 ks 字节级行为）|
| **自写静态 ARM64 Sahara 加载器** | ✅ 12 个固件镜像**全部被 modem 接受**（每个 `status=0`）|
| 完整 ramdump（14 个内存区） | ✅ 完成 |
| 传输性能 | ✅ 提升 28×（~250 KB/s → **~7 MB/s**，61.6 MB / 8.8 s）|
| 内核 ks_bridge 聚合转发（让原厂 ks 可用） | ✅ NAK 归零（`nak=0`）|
| **modem 执行固件 → 进入 AMSS** | ❌ **卡点**：收完全部镜像后停在 PBL，`MDM2AP_STATUS` 始终为低 |
| QMI/RIL 用户态移植（读卡前提）| ❌ 未开始 |
| SIM / 网络锁 | ❌ 未开始 |

> ⚠️ 重要事实：**这台机器的 SIM 卡座接在外挂 MDM9635 上**（不在 AP 上），
> 所以"读卡/上网"必须先把 modem 跑起来；而 LOS 20 的 vendor 来自**国际版 S6（Shannon 基带）**，
> 因此即使 modem 起来，也还需移植高通 QMI/RIL 栈。

---

## 交付物

| 文件 | 说明 |
|---|---|
| `docs/G9209-MDM9635-基带逆向-完整技术文档.md` | **完整技术文档**（协议逆向、工具链、内核改动、实验规程、复现指南、证据清单）|
| `tools/sahara.c` | **自写静态 ARM64 Sahara 加载器**（v39：握手 / 整块服务 / ramdump / 自注册 ESOC req engine / 自愈）|
| `tools/*.c` | 嗅探器、ESOC 上电、HSIC ready、ks 转发器等配套工具 |
| `patches/` | 内核补丁（5 个文件，见下）|
| `evidence/` | 关键证据（modem 交出的内存区表、复位原因、驱动事件日志等）|
| Releases 中的 `g9209-mdm9635-lineage-20.0-*.zip` | **TWRP 可刷包**：LineageOS 20（原 fakeman 包）+ **本项目内核**（boot-AD）|

### 内核补丁（`patches/`）
| 补丁 | 作用 |
|---|---|
| `drivers_pci_host_pci-exynos.c.patch` | PCIe 链路失败不再 panic（否则开机卡第一屏）|
| `drivers_esoc_esoc-mdm-drv.c.patch` | modem 启动窗口 60s → **900s**；超时不再 panic |
| `drivers_esoc_esoc_dev.c.patch` | `ESOC_PANIC` → `-EINVAL` |
| `arch_arm64_mach-exynos_subsystem_restart.c.patch` | 5 处 panic → 打印；恢复 `RESET_SOC` 的复位调用 |
| `drivers_usb_misc_ks_bridge.c.patch` | **写路径聚合转发**：原厂 ks 以 4 字节粒度写，驱动替它按 Sahara 头/请求边界重新分块（NAK 5/8 → 0）|

---

## 刷机（TWRP）

**前置**：已解锁 BL + 已装 TWRP；**务必先备份**（本机已备份 EFS：`m9kefs1/2/3` 与 RADIO）。

1. 下载 Releases 里的 `g9209-mdm9635-lineage-20.0-*.zip`
2. 复制到手机 → 进 TWRP → **Install** → 选择该 zip → 滑动刷入
3. 该包 = 原 LineageOS 20（`system`）+ **本项目内核**（`boot.img`，含全部补丁）

> 说明：这个内核补丁集**是这台机器能正常启动 LOS 20 的前提**（原版内核会卡在第一屏）。
> 刷回原厂：用 Odin/heimdall 刷原厂 4 件套（AP/BL/CP/CSC_CTC）即可。

---

## 复现研究（无需刷机）

```bash
adb push tools/sahara /data/local/tmp/mdm/ldrx && adb shell chmod 755 /data/local/tmp/mdm/ldrx
adb shell 'setprop ctl.stop ril-daemon; setprop ctl.stop cpboot-daemon'   # 关键：否则 init 每 5s 杀进程组
adb shell 'mkdir -p /firmware; mount -t vfat -o ro /dev/block/platform/15570000.ufs/by-name/RADIO /firmware
           mkdir -p /dev/block/modem; for p in m9kefs1 m9kefs2 m9kefs3; do ln -sf /dev/block/platform/15570000.ufs/by-name/$p /dev/block/modem/$p; done
           mount -o remount,rw /; printf "#!/system/bin/sh\nexit 0\n" > /system/bin/ks'
adb reboot    # modem 需要一次全新的上电才会枚举 USB
# 开机后：
adb shell 'nohup setsid /data/local/tmp/mdm/ldrx >/dev/null 2>&1 </dev/null &'
adb shell '/data/local/tmp/mdm/powerup /dev/subsys_esoc0 & sleep 8; /data/local/tmp/mdm/esoc_hsic'
adb shell 'tail -f /data/local/tmp/diag/sahara_own.log'   # 期待 12 个 END_IMAGE_TX status=0
```

判据：
- 镜像加载成功：日志出现 12 个 `END_IMAGE_TX id=… status=0`
- ramdump 成功：`/data/local/tmp/diag/cpdump/*.BIN`（14 个，约 444 KB）
- **AMSS 起来（尚未达成）**：USB 出现 AMSS 阶段 PID（0x9048/0x904C/0x9075…）+ `/dev/efs_hsic_bridge` + `MDM2AP_STATUS=hi`

---

## 免责声明

* 本项目为**个人设备的研究记录**，仅供学习与逆向研究使用；**不提供任何破解/解锁服务**。
* 刷机有风险（可能变砖、丢数据），请自行评估并**先备份**。
* 仓库内**不包含**任何三星/高通专有二进制固件；请从你自己的设备/官方固件包中获取。
* 涉及的商标与固件版权归其各自所有者。
