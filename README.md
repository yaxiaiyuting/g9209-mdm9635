# SM-G9209 (Galaxy S6 电信版) 外挂高通 MDM9635 基带 —— 在 LineageOS 20 上的适配

> 机型：Samsung SM-G9209（`zerofltectc`，Exynos7420 AP + **外挂** Qualcomm MDM9635 modem）
> 目标：LineageOS 20（Android 13，`zeroflte`）
> 目标功能：读卡 / 移动数据 / 短信 / 语音通话（VoLTE 不作为目标）

---

## 0. 这台机器特殊在哪

**SIM 卡座物理上挂在 MDM9635 上，不接 AP。** 所以 modem 不跑起来，就绝无可能读卡。

```
┌──────────────────────────┐   HSIC    ┌───────────────────────────┐
│ Exynos7420 (AP)          │◄─────────►│ Qualcomm MDM9635          │
│  - 无集成基带 ✗           │           │  - PBL / SBL1 / TZ / RPM  │
│                          │           │  - AMSS (qdsp6sw, 56MB)   │
│                          │           │  - ★ SIM 卡座挂在这里      │
└──────────────────────────┘           └───────────────────────────┘
```

外挂 modem **没有自己的 flash**：每次上电，AP 都必须通过 HSIC 把 12 个固件镜像
（共约 67 MB）喂给它，EFS/NV 也由 AP 侧分区代理。

---

## 1. 当前状态（诚实评估）

| 阶段 | 状态 | 证据 |
|---|---|---|
| Sahara 协议逆向 | ✅ 完成 | 12 个镜像全部 `END_IMAGE_TX status=0` |
| 自写静态 ARM64 加载器 | ✅ 可用 | 67 MB / 约 20 秒（约 21 MB/s），零错误 |
| 内核适配（能正常启动） | ✅ 可用 | 见 §4 内核改动 |
| **modem 启动到 AMSS** | ✅ **已达成** | dmesg：`status = 1: mdm is now ready` |
| ESOC `boot_done` / subsys ONLINE | ✅ 已达成 | `powerup` 解除阻塞、`subsys0/state=ONLINE` |
| AP 侧 `AP2MDM_HSIC_READY` 可控 | ✅ 已证实 | 内核 printk 实测 `before=0 → after_set1=1` |
| **AMSS 重新枚举出复合设备** | ❌ **当前卡点** | `ksb_usb_probe` 只见 `pid=9008` |
| `/dev/efs_hsic_bridge` | ❌ 从未出现 | — |
| QMI/RIL 用户态 | ⬜ 未开始 | — |
| 读卡 / 上网 / 短信 / 通话 | ⬜ 未开始 | — |

**一句话**：主机侧（AP）已全部打通，modem 也确实启动到 AMSS；**唯一卡点是 modem 不把自己的
USB 切成 AMSS 形态**，因此 AP 侧拿不到 `/dev/efs_hsic_bridge`，也就无法做 EFS 同步、无法进 RIL。


---

## 1b. ★ 决定性对照（2026-10-03）：原厂 `mdm_helper` 也失败在同一处

为了排除"是不是我们自写的主机侧实现有问题"，我们把 `/system/bin/ks` 换成
[`tools/ks_passthru.c`](tools/ks_passthru.c)（逐字转发原厂 `mdm_helper` 的 argv，
只丢掉 `-r`/`--ramdumpimage`/`-q` 这几个开关），然后**让原厂 `mdm_helper` 驱动整条链路**。

| 路线 | 加载 12 镜像 | 到 AMSS | 重新枚举 | 结果 |
|---|---|---|---|---|
| 自写加载器 + 自写握手 | ✅ 20 秒 | ✅ | ❌ | 约 11.2 s 后自复位 |
| **原厂 `mdm_helper` + `ks.real`** | ✅ | ✅ | ❌ | **同样约 11.2 s 后自复位** |

**两条完全不同的主机侧实现得到完全相同的失败现象** → 问题不在 AP 侧实现。

### modem 自己给出的证据

`/cpdump/mdm_err.log`（kickstart dump 出来的 modem 崩溃日志）：

```
There is not valid Crash Reason, so I guess this crash happen before running
the err_init() on apps_proc !
```

即 **崩溃发生在 modem 错误处理器 `err_init()` 初始化之前** —— 极早期。
配合：`MDM_ERR_FATAL.BIN` 全零、`MDM2AP_ERRFATAL` 全程 `lo`（无软件 fault）、
`RST_STAT=2`、`PmicPONstat=20 00 02 00 02 00 00 00`（有置位）、
失败时间常数极稳（≈11.2 s，方差 <0.05 s，硬件计数器特征）。

→ **modem 在 AMSS 极早期被看门狗/受控复位，从未走到"打开自己 USB PHY 并枚举"。**

### 已穷尽排除的 AP 侧变量

| 变量 | 做法 | 结果 |
|---|---|---|
| `HSIC_READY` 没被调用 | 内核 printk 实测 | 确实被调用，`after_set1=1` |
| `HSIC_READY` 时机太晚 | 提前到 STATUS 后 / Sahara 前 / **上电瞬间并全程保持** | 三种都不枚举 |
| EHCI 不重新枚举 | unbind→10ms→bind | 两次写都成功，仍只有 9008 |
| req engine 无人持有 | 常驻 daemon | 已修 |
| `ESOC_BOOT_DONE` 未发 | 已实现 | 已修 |
| `powerup` 退出关 fd | 持 fd 常驻 | 已修 |
| 主机实现有问题 | **原厂 `mdm_helper` 对照** | 同样失败 |
| 镜像内容 | 与原厂 CP 包 md5 9/9 一致 | 排除 |

### 下一步唯一有希望的方向

**把 modem 侧 AMSS 的早期日志引出来**（DIAG / 串口），看它在 `err_init()` 之前
究竟执行到哪、等的是什么。这是目前唯一还没打开的黑盒。

---

## 2. ★ 对既有结论的重要更正

前一份技术文档（`G9209-MDM9635-基带逆向-完整技术文档.md`）有两处核心判断需要更正，
否则会把人带向错误方向：

### 2.1 `HELLO mode=2` 不是"命令模式"

按[高通 Sahara 协议规范](https://lore-kernel.gnuweeb.org/linux-arm-msm/ab548ca1-c758-4096-b6aa-bce886fd904f@oss.qualcomm.com/T/#u)：

| mode | 含义 |
|---|---|
| 0 | IMAGE_TX_PENDING（还有镜像要传） |
| **1** | **IMAGE_TX_COMPLETE（镜像已全部传完）** |
| 2 | MEMORY_DEBUG（内存转储） |
| **3** | **COMMAND（命令模式）** |

原文档把 `mode=2` 记作"命令模式"，于是把剩余工作定为"实现命令模式"。
**方向错了** —— 实测拿到的是 **`mode=1`**，即 modem 自己宣告"镜像已收齐"，Sahara 层本就已完成。

### 2.2 "modem 收完固件不执行"不成立

实测 dmesg（自写加载器 v40，已移除一切自我复位逻辑）出现：

```
[101.203] ext-mdm qcom,mdm1.54: status = 1: mdm is now ready
```

**这一行在原文档中从未出现过。** modem 的 AMSS 确实启动了。

---

## 3. 已定位并修掉的三个真实根因

这三个都是原文档完全没有意识到的前提，缺一个都跑不通。

### 3.1 必须有进程**常驻持有 ESOC request engine**

内核把 modem 的请求投进 req fifo，**只有注册为 request engine 的进程**能取走
（`ESOC_WAIT_FOR_REQ`）。无人持有 → PBL 永远不发 Sahara HELLO
（表现为：端口能打开却收不到任何数据，同时 EHCI 报 `XactErr len 0/16384`）。

`dmesg` 实证：`Signaling request engine for images` 之后 modem 才开始发 HELLO。

→ 工具：`tools/esoc_reqeng.c`（常驻；父进程持 req engine，另 fork 子进程抽干 evt fifo）

### 3.2 `ESOC_BOOT_DONE` **只能由用户态发**，内核从不发

`drivers/esoc/esoc-mdm-4x.c:563` 的 `case ESOC_BOOT_DONE:`
是 `ESOC_RUN_STATE` 事件的**唯一**来源；它到达 `esoc-mdm-drv.c:65` 才 `complete(&boot_done)`。
不发 → `mdm_subsys_powerup()` 永久阻塞、`subsys0/state` 永远 OFFLINE。

原厂 `mdm_helper` 里有这段（其 strings 可见 `Failed to send ESOC_BOOT_DONE notification`）。
→ 已实现。

### 3.3 `powerup` 进程退出会关 fd → 内核把 modem 打掉

`subsys_device_close()`（`arch/arm64/mach-exynos/subsystem_restart.c:909`）
→ `subsystem_put()` → 引用计数归零 → `mdm_subsys_shutdown()` → `ESOC_PWR_OFF`。

实测：`powerup` 一退出，紧接着就是
`Graceful shutdown fail, ret = -19` + `Doing a hard reset` —— 把自己刚启动的 modem 关掉。

→ 工具：`tools/esoc_pwron_hold.c`（上电后**常驻持 fd**）

### 3.4 附带发现：旧加载器在"自伤"

`tools/sahara.c` v39 在最后一个镜像的 `DONE_RESP` 之后会调用 `ESOC_SET_CRASH`
并写 `restart` 到 `/sys/kernel/debug/msm_subsys/esoc0`，**主动复位刚启动的 modem**。
（这段是从原厂 `ks` 的 *ramdump 路径* 误抄到正常启动路径上的。）
→ v40 已移除。

---

## 4. 内核改动

补丁：[`patch/mdm-bringup.patch`](patch/mdm-bringup.patch)

| 文件 | 改动 | 原因 |
|---|---|---|
| `arch/arm64/mach-exynos/subsystem_restart.c` | 5 处 `panic()` → `pr_err`；恢复 `RESET_SOC` 分支的 `__subsystem_restart_dev(dev)` | 前者防开机卡第一屏；后者让用户态能复位 modem |
| `drivers/esoc/esoc-mdm-drv.c` | `boot_done` 等待 60 s → 900 s；超时 `panic()` → `-EIO` | flashless boot 逐镜像会话耗时长 |
| `drivers/esoc/esoc_dev.c` | `ESOC_PANIC` → `-EINVAL` | 防用户态误触发 panic |
| `drivers/esoc/esoc-mdm-4x.c` | 加 `[MDM-DIAG]` printk（诊断 `set_hsic_ready` 的 GPIO 实际电平） | 定位 HSIC_READY 是否真被驱动 |
| `drivers/pci/host/pci-exynos.c` | PCIe 链路失败 `panic()` → 打印错误并返回 | 否则开机即卡第一屏 |
| `drivers/usb/misc/ks_bridge.c` | TX 路径小包聚合转发（按 modem `READ_DATA` 请求长度分块） | 原厂 `ks` 以 4 字节为粒度写，直接转发会被判 `NAK` |

---

## 5. ★★ 刷机安全规矩（血泪教训，务必遵守）

### 5.1 绝不能把 CHN DTB 打进 boot.img

Assembly script 会把编译出的 **5 个 CHN DTB**（`exynos7420-zeroflte_chn_0{0..4}.dtb`，
hw_rev `0..6 / 7 / 8 / 9 / 10..255`）装进 boot.img 的 DTBH tail。
**这套 CHN DTB 在本社区内核上根本起不来** —— 已三次复现：

- `boot-H-chn-factory.img`、`boot-L-chn-fixed-kernel.img`：各等 5 分钟无 adb
- `boot-AE-diag.img`：刷入后手机**从 USB 总线完全消失**（连 Download 模式都没有），
  最后靠进 TWRP 手工 `dd` 才恢复

**正确做法**：**沿用 `mdm-work/boot-AD-reqleft.img`（已知可启动）的 DTB tail，只替换内核**。
实测 `boot-AF-newkern-adtail.img` 正常启动（内核 `#27`）。
→ 用 [`mdm-work/make_boot_safe.py`](../../baseband-study/mdm-work/make_boot_safe.py)：默认沿用 AD 的 tail、
校验基座 md5、重建 DTB 必须显式 `--rebuild-dtbs` 且告警、带完整回读校验。

### 5.2 刷机流程

```bash
cd baseband-study
python3 mdm-work/make_boot_safe.py mdm-work/boot-new.img   # ★ 只用这个脚本
adb push mdm-work/boot-new.img /data/local/tmp/boot-new.img
adb shell 'md5sum /data/local/tmp/boot-new.img'            # 校验
adb shell 'dd if=/data/local/tmp/boot-new.img of=/dev/block/sda8 && sync'
adb shell 'md5sum /dev/block/sda8'                         # ★ 必须回读校验
adb reboot
```

**救命镜像**：`mdm-work/boot-AD-reqleft.img`（md5 `5e575b6a0eb83eb5bd1d731349798d9d`）——切勿覆盖。
恢复流程见 [`baseband-study/exp/RECOVERY-从AE回到AD.md`](../../baseband-study/exp/RECOVERY-从AE回到AD.md)。

### 5.3 每轮实验前必须重启

modem 只在**一次新的 ESOC 上电**之后才重新枚举 HSIC/USB；
`powerup` 在 modem 已上电时是**空操作**。
另外要先 `echo DISABLE > /data/local/tmp/diag/DISABLE` 关掉 `cbd` 开机钩子
（否则它会每 5 秒重生 `mdm_helper`，与我们的加载器抢端口/抢 req engine）。

---

## 6. 工具链

主机侧源码在 [`baseband-study/tools/`](../../baseband-study/tools/)：

| 工具 | 作用 | 关键点 |
|---|---|---|
| `sahara.c` (v40) | 自写静态 ARM64 Sahara 加载器 | 12/12 `status=0`；21 MB/s；**已移除自我复位** |
| `esoc_reqeng.c` | 常驻 ESOC 请求引擎 | 父进程持 req engine + 子进程抽干 evt fifo |
| `esoc_pwron_hold.c` | 上电并**常驻持 fd** | 否则内核会 `PWR_OFF` 打掉 modem |
| `mdm_runtime_handshake.c` | 复刻原厂 `MODE_RUNTIME` | EHCI unbind/bind → `HSIC_READY` → 等 efs 桥 → `BOOT_DONE` |
| `mdmdump.c` | 经 Sahara 读 modem 内存/寄存器 | ⚠️ 见下方限制 |
| `sniff.c` | `/dev/ks_hsic_bridge` 原始字节嗅探 | 协议帧格式就是它抓出来的 |

编译（静态、无 libc、纯 syscall）：

```bash
GCC=/home/duanjb666/los20/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin/aarch64-linux-android-gcc
$GCC -static -O2 -std=gnu99 -nostdlib -ffreestanding -fno-stack-protector -o tools/xxx tools/xxx.c
```

---

## 7. 已排除项（省后来者时间）

| 怀疑 | 结论 | 证据 |
|---|---|---|
| 镜像内容错 | ✗ 排除 | 设备 9 个镜像与原厂 CP 包 md5 **9/9 一致** |
| EFS 内容错 | ✗ 排除 | `m9kefs1/2/3` 带三星 `IMGEFS1$` 头、真实数据 |
| DTB 配置错 | ✗ 排除 | CHN 与 EUR 的 `qcom,mdm1` 节点结构完全一致；`ap2mdm-hsic-ready-gpio = <&gpf2 3>` = gpio-135 与实测一致 |
| 传输协议 | ✗ 排除 | 12 镜像全 `status=0`，21 MB/s，零错误 |
| 内核不稳定 | ✗ 排除 | 连续多轮 `panic=0` |
| RSA/签名 | ✗ 排除 | `ksb_usb_probe` 正常绑定 `pid=9008 ifc=0` |
| **USB 停在 9008 是故障** | ✗ **这不是判据** | 固件里根本不存在 `9048/904C/9075`；但内核 `ks_bridge.c` 认这些 PID → **AMSS 起来后本应重新枚举** |
| HSIC_READY 没被调用 | ✗ 排除 | printk 实测 `gpio=135 before=0 valid=1` → `after_set1=1` |
| HSIC_READY 时机太晚 | ✗ 排除 | 把它提前到 Sahara 之前（`EXP11 early`）仍然不枚举 |
| `peripheral_reset` 有隐藏动作 | ✗ 排除 | 反汇编证实**就是** unbind→10ms→bind，无任何额外动作 |

---

## 8. 当前卡点的完整证据链

```
[  ~54s ] 12 个镜像全部 END_IMAGE_TX status=0        ← Sahara 完成
[  ~54s ] modem 发 HELLO mode=1                      ← 自己宣告"镜像收齐"
[  ~66s ] ext-mdm: status = 1: mdm is now ready      ← ★ AMSS 起来了
           ↓ 此刻 USB 上仍然是 05c6:9008（PBL 身份），全程不断开
[  ~72s ] [MDM-DIAG] set_hsic_ready ENTER gpio=135 before=0 valid=1
[  ~72s ] [MDM-DIAG] set_hsic_ready EXIT after_set1=1 ← ★ 引脚确实被拉高
[  ~72s ] EHCI unbind/bind 成功（两次写都返回 12）
[  ~77s ] esp USB 重新枚举 …… 但仍然只有 pid=9008
[  ~84s ] ksb_usb_probe ENTER vid=05c6 pid=9008       ← 没有任何 AMSS 设备
[  ~88s ] STATUS -> lo                                ← modem 自复位（约 11 秒）
```

**且 `MDM2AP_ERRFATAL` 全程为 `lo`** —— modem 从未进入 ERR_FATAL 路径。
另有独立证据：原厂 `MDM_ERR_FATAL.BIN`（4224 B）**非零字节数为 0**，
即"复位被引导链记录了，但没有任何软件故障记录" —— 这是**硬件看门狗复位**的典型特征。

modem 固件（`apps.mbn`，构建路径 `ZEROFLTE_CHN_CTC`，正是本机）里有：

```
SS_Hsic_host_ready_Raising_Signal_Isr / Falling_Signal_Isr
SS_drive_Device_Ready_Gpio
 - Ignore because Host ready GPIO is Low
 - HSIC DEVICE_READY Setting HIGH, USB PHY ON : ready=%d
 - Rearm Timer : is_host_ready_recvd=%d, is_hwgpio_timer_started=%d
hsu_bam_sm__enumeration_with_ap / hsu_bam_sm_notify_host_ready
mhi_core_link_completion_timer_cb: Timeout
sys_m_task() system:reset
```

→ 即 AMSS 把"打开自己 USB PHY / 拉起 DEVICE_READY / 发出 HSIC connect"
门控在某个握手上，并用一个会 Rearm 的定时器跟踪。
AP 侧已证实把 `AP2MDM_HSIC_READY` 拉高，但 modem 仍不开 USB PHY。

---

## 9. 下一步（尚未完成）

1. **★ 把 modem 侧 AMSS 日志引出来**（DIAG / 串口）—— 这是目前唯一没打开的黑盒，
   能直接看到它卡在 HSIC 状态机哪一步。
   ⚠️ 已知限制：modem 启动过之后再想用 Sahara 读它内存是**行不通的**
   （`mdmdump` 实测卡在等 HELLO —— 只有 PBL 主动发起握手时才行）。
2. **对照原厂内核**：原厂 boot.img 里 `esoc-mdm-4x.c` / `ks_bridge.c` 与社区内核的差异点。
   原厂能跑通，差异点很可能就是答案。（本项目已有的原厂 kernel 是 stripped 的，需另找带符号版本。）
3. 补齐原厂 `MODE_BOOT` 链路建立：
   `unbind → ESOC_NOTIFY(4=IMG_XFER_RETRY) → WAIT_FOR_REQ → usleep(500ms) → bind`
   （原厂靠它让每轮都重新武装 `mdm->init`）。
4. `ESOC_IMG_XFER_DONE` 移到 Sahara **之后**发（原厂如此）。
5. efs 等待从 15 s 拉到 **37.5 s**（原厂 `WaitForCOMport(...,75,...)`，单位 0.5 s），
   且**桥不出现就不发 `ESOC_BOOT_DONE`**。
6. EFS 同步：`ks -m -p /dev/efs_hsic_bridge -w /dev/block/modem/ -t -1 -l`
   （`ks.real` 支持 `-l/--efssyncloop`）。

---

## 10. 引用与来源

- [高通 Sahara 协议规范（Linux kernel 补丁系列 v1，含完整 `Documentation/sahara/sahara_protocol.rst`）](https://lore-kernel.gnuweeb.org/linux-arm-msm/ab548ca1-c758-4096-b6aa-bce886fd904f@oss.qualcomm.com/T/#u)
- [MDM9x25 Flashless boot & IPC over HSIC（同族机型原厂 C 源码，含 `configure_flashless_boot_dev()` 全文）](https://blog.csdn.net/hongzg1982/article/details/54884883)
  - 镜像（未被截断）：<https://www.vevb.com/wen/2019/11-10/149752.html>

本项目内部的详细分析报告（均在 `baseband-study/exp/`）：
`STATUS-当前进展.md`（主文档）、`FINDINGS-vendor-sequence.md`（原厂 mdm_helper 逆向）、
`FINDINGS-amss-boot.md`、`FINDINGS-firmware-dump.md`。

---

## 11. 许可与免责

本项目为**个人设备研究记录**，包含逆向工程结论。
所有实验均在自有设备上进行。刷机有风险，请自行备份并保留可回滚镜像。
EFS/NV 与网络锁相关内容请遵守当地法律法规。
