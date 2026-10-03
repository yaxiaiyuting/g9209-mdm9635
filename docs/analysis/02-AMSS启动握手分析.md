# FINDINGS：MDM2AP_STATUS 拉高之后 AP 还欠 modem 什么？

> 交付对象：SM-G9209（`zerofltectc`）外挂 MDM9635 bring-up
> 分析范围：**纯静态分析**（本机内核源码 + 原厂 userspace 二进制反汇编 + 厂包固件字符串 + 公开资料）
> 未使用 adb、未接触真机、未修改任何内核源码。
> 日期：2026-10-03

---

## 0. 结论（TL;DR）

**原厂 `mdm_helper` 的流程在「MDM2AP_STATUS 拉高」之后还有 4 个必须动作，当前实验流程一个都没做：**

| # | 动作 | 机制 | 缺失后的现象 |
|---|---|---|---|
| **A** | **HSIC EHCI 控制器 unbind + 10ms + bind** | `echo 15510000.usb > /sys/bus/platform/drivers/s5p-ehci/unbind` → `usleep(10000)` → `echo 15510000.usb > /sys/bus/platform/drivers/s5p-ehci/bind` | AP 侧 EHCI 根端口仍锁在 PBL 阶段的 `05c6:9008` 设备上，**AMSS 阶段的复合 USB 设备永远不会被枚举** → 无 `/dev/efs_hsic_bridge`、无 `hsic_sysmon`、无 `diag_bridge` |
| **B** | **`ioctl(/dev/esoc-0, ESOC_SET_HSIC_READY /*0xcc0c*/)`** | `mdm_set_hsic_ready()` 把 `AP2MDM_HSIC_READY` 拉低 10ms 再拉高，产生一个**上升沿** | modem 侧 `apps.mbn` 里的 `SS_Hsic_host_ready_Raising_Signal_Isr` / `is_host_ready_recvd` 握手收不到边沿 |
| **C** | **等 `/dev/efs_hsic_bridge` 出现并跑 EFS 同步**：`/system/bin/ks -m -p /dev/efs_hsic_bridge -w /cpdump/ -t -1 -l -g <prefix>` | modem 自身**没有 flash**，它的 EFS/NV 存在 AP 的 `m9kefs1/2/3` 分区上，必须由 AP 通过 HSIC EFS 桥服务 | AMSS 的 EFS 同步超时 → 判定 boot 失败 |
| **D** | **`ioctl(/dev/esoc-0, ESOC_NOTIFY, {ESOC_BOOT_DONE=2})`** | `mdm_notify()` → `esoc_clink_evt_notify(ESOC_RUN_STATE)` → `complete(&boot_done)` | AP 侧 `mdm_subsys_powerup()` 永远阻塞（这就是为什么要把 60s 改成 900s——**那是症状不是解法**），`subsys0/state` 永远 OFFLINE |

**根因（最可能）**：modem 的 AMSS/APPS 固件在拉高 `MDM2AP_STATUS` 后进入一个「等 host ready / 等与 AP 完成枚举」的状态机（firmware 字符串实证：`hsu_bam_sm__enumeration_with_ap`、`hsu_bam_sm_notify_host_ready`、`SS_Hsic_host_ready_Raising_Signal_Isr`、` - Rearm Timer : is_host_ready_recvd=%d, is_hwgpio_timer_started=%d`、`hsu_conf_sel_stack_utils_device_restart_core`、`sys_m_task() system:reset`）。AP 侧不完成 A+B 就永远不枚举，modem 的定时器到期后**受控地把 `MDM2AP_STATUS` 拉低并自复位**（所以 `MDM2AP_ERRFATAL` 一直是 lo，没有 errfatal）。11.2s 就是那个 modem 内部定时器。

**最关键的一条**：`AP2MDM_HSIC_READY` 现在是**在 Sahara 之前就被 `esoc_hsic` 拉高了**（而且 `ESOC_PWR_ON` 本来是把它清 0 的）。原厂是在 STATUS 拉高**之后**才第一次拉高它 —— 也就是说 modem 期望的是一个**上升沿**，而不是一个一直为高的电平。

---

## 1. 铁证：原厂 `mdm_helper` 的完整流程（本机反汇编）

### 1.1 二进制

本机就有原厂文件（**不需要真机**）：

```
baseband-study/stock/qcom-bin/mdm_helper        (27272 B, aarch64, stripped, md5 6d2bbae972ccbc90f2c23bb3219b5405)
baseband-study/stock/qcom-bin/mdm_helper_proxy  (md5 83f5e9e78bbd860d2369c52678db6205)
baseband-study/port-blobs/vendor/bin/mdm_helper (同一文件)
baseband-study/port-blobs/system/lib64/libmdmdetect.so
baseband-study/stock/qcom-bin/{cbd,qmuxd,rild,irsc_util,...}
baseband-study/stock/init.baseband.rc           ← 原厂 init 脚本（含 mdm_helper 服务定义）
```

反汇编命令：
```bash
/home/duanjb666/los20/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin/aarch64-linux-android-objdump \
    -d --no-show-raw-insn baseband-study/stock/qcom-bin/mdm_helper > /tmp/mdm_helper.asm
# .rodata vaddr == file offset（0x3ef0~0x53be），所以 strings -t x 的偏移就是虚拟地址
```

### 1.2 反汇编还原出的 `mdm9k_powerup()` 完整时序

（地址均为 mdm_helper 内偏移；`x19 = struct mdm_device *dev`，`[dev+272] = pdata`）

```
2698  mdm9k_powerup(dev):
26d8    property_get("persist.mdm_boot_debug", buf, "false")
277c    组装 ks argv（EFS 同步用）: {"/system/bin/ks"(0x48ca), "-m", "-p", pdata[3], "-w", pdata[10],
                                    "-t", "-1", "-l", "-g", pdata[9], NULL}
2844    fd = open(dev->esoc_node, O_RDWR)          → "/dev/esoc-0"
2858    ioctl(fd, 0x0000CC07)                       → ESOC_REG_REQ_ENG     ★必须
2864    ioctl(fd, 0x8004CC02, &req)                 → ESOC_WAIT_FOR_REQ   等待 ESOC_REQ_IMG
2880    if (req != 1 /*ESOC_REQ_IMG*/) → error
2890    configure_flashless_boot_dev(dev, 1)        → MODE_BOOT   ★★★ (见 1.3)
2894    if (ret) → "Link setup failed"
       ── 这里才 fork/exec /system/bin/ks 喂 12 个镜像（原厂实测命令行见 §1.4）──
2ad4    ioctl(fd, 0x4004CC03, &{1})                 → ESOC_NOTIFY(ESOC_IMG_XFER_DONE)
2af4    for (i=0;i<61;i++) {                        ← 最多 61 次
2b00      ioctl(fd, 0x8004CC04, &status)           → ESOC_GET_STATUS
2b1c      if (status == 1) goto 2bc0
2b40      usleep(1000000)                          ← 1 秒
        }
2bf4    log "MDM did not set MDM2AP_STATUS high"
2c18    ioctl(fd, ESOC_NOTIFY, &{3})               → ESOC_BOOT_FAIL;  return FAILED
2bc0    log "%s: MDM2AP_STATUS is now high"        ★★ 这就是我们观测到的 101.203 行
2c4c    if (!strncmp(dev->link, "HSIC", 5))
2c64      configure_flashless_boot_dev(dev, 2)     → MODE_RUNTIME  ★★★ 关键缺失段 (见 1.3)
2cd0    port = pdata[3] /*"/dev/efs_hsic_bridge"*/
        if (!port) log "No efs_sync_device specified for target"
        else if (WaitForCOMport(port, 75, 0)) log "Could not detect EFS sync port"
2d10    pid = fork()
2d20      child: execve("/system/bin/ks", argv/*EFS 同步 argv*/, NULL)
2d38    ioctl(fd, ESOC_NOTIFY, &{2})               → ★ ESOC_BOOT_DONE
2d58    fd2 = open("/dev/diag", O_RDWR)
2dd0      if (fd2 < 0) ioctl(fd, ESOC_NOTIFY, &{12}) → ESOC_DIAG_DISABLE
        return SUCCESS
```

`struct mdm_private_data`（`[dev+272]`）里被用到的字段：
`+8 = peripheral_cmd()`（bind/unbind）、`+16 = ops[]`、`+24 = flashless_boot_device`（`/dev/ks_hsic_bridge`）、
`+72 = efs prefix`、`+80 = workdir`（`/cpdump/`）、`+88 = efs_sync_pid`。

### 1.3 `configure_flashless_boot_dev(dev, mode)` —— 缺失的就是 `MODE_RUNTIME` 分支

反汇编（函数入口 `0x2e84`）：

```
2e84  configure_flashless_boot_dev(dev /*x0*/, mode /*w1*/):
2ea8  w20 = mode
2eb4  w8 = 4 ; str w8,[sp,#20]        → cmd = ESOC_IMG_XFER_RETRY (=4)
2ec4  str xzr,[sp,#8]                → req = 0
2ecc  x28 = pdata = [dev+272]
2ed8  strncmp(dev->link /*dev+0x40*/, "HSIC"(0x416a), 5);  if (!=0) → "Link %s not supported by mdm-helper"

2fb8  if (mode == 1 /*MODE_BOOT*/ || mode == 3 /*MODE_RAMDUMP*/):
2fc4    log "%s: Setting up %s boot link"                          (0x51ed)
3020    for (i = 0; i < 50; i++) {
3028      log "%s: %s: Initiating HSIC unbind"                     (0x4ce0)
3058      pdata->peripheral_cmd(dev, 2 /*PERIPHERAL_CMD_UNBIND*/)
3064      ioctl(esoc_fd, 0x4004CC03 /*ESOC_NOTIFY*/, &cmd /*=4*/)
307c      ioctl(esoc_fd, 0x8004CC02 /*ESOC_WAIT_FOR_REQ*/, &req)
3094      if (mode != 3 && req != ESOC_REQ_IMG) { log "Unnexpected request"; continue; }
30dc      usleep(500000)                                          ← 0.5 s
30f0      log "%s: %s: Initiating HSIC bind"                       (0x5249)
3120      pdata->peripheral_cmd(dev, 1 /*PERIPHERAL_CMD_BIND*/)
312c      rcode = WaitForCOMport(pdata->flashless_boot_device, 5, 0)
313c      if (rcode == 0) return SUCCESS
        }
3158    log "%s: %s: Failed to setup HSIC link"                    (0x5266); return FAILED

316c  else if (mode == 2 /*MODE_RUNTIME*/):        ★★★★★ 完全缺失
3174    log "%s: Setting up %s link for efs_sync"                  (0x5288)
31a8    x20 = pdata
31ac    if (pdata->peripheral_cmd) {
31b4      pdata->peripheral_cmd(dev, 2)      ← UNBIND
31c0      usleep(10000)                     ← 10 ms
31cc      pdata->peripheral_cmd(dev, 1)      ← BIND       ★ 强制重新枚举 AMSS 阶段设备
        }
31d8    log "%s: Sending boot status notification to HSIC"        (0x52ac)
3204    ioctl(esoc_fd, 0x0000CC0C /*ESOC_SET_HSIC_READY*/)        ★ HSIC_READY 上升沿
3210    if (rc < 0) log "%s: %s:hsic_ready failed"                 (0x52d9); return FAILED
        return SUCCESS

321c  else return SUCCESS
```

`peripheral_cmd()` 的实现（字符串 + 反汇编一致）：

```c
case PERIPHERAL_CMD_UNBIND:
    fd = open("/sys/bus/platform/drivers/s5p-ehci/unbind", O_WRONLY);
    write(fd, "15510000.usb", strlen("15510000.usb"));
case PERIPHERAL_CMD_BIND:
    fd = open("/sys/bus/platform/drivers/s5p-ehci/bind", O_WRONLY);
    write(fd, "15510000.usb", strlen("15510000.usb"));
```
（对应字符串常量：`0x4608 "/sys/bus/platform/drivers/s5p-ehci/bind"`、
`0x4630 "/sys/bus/platform/drivers/s5p-ehci/unbind"`、`0x465a "15510000.usb"`、
`0x5369 "Failed to open bind node : %s"`、`0x5387 "Failed to write to bind node: %s"`）

**这就是「STATUS 拉高之后本应执行但被跳过」的分支。**

### 1.4 原厂 `ks` 命令行（两段）

**Boot 段（本仓库已实测抓到）**：
```
/system/bin/ks -w /cpdump/ -p /dev/ks_hsic_bridge -r 21 \
   -s 21:/firmware/image/sbl1.mbn -s 25:/firmware/image/tz.mbn \
   -s 30:/firmware/image/sdi.mbn  -s 23:/firmware/image/rpm.mbn \
   -s 31:/firmware/image/mba.mbn  -s 8:/firmware/image/qdsp6sw.mbn \
   -s 28:/firmware/image/dsp2.mbn -s 6:/firmware/image/apps.mbn \
   -s 16:/dev/block/modem/m9kefs1 -s 17:/dev/block/modem/m9kefs2 \
   -s 20:/dev/block/modem/m9kefs3 -s 29:/firmware/image/acdb.mbn
```

**EFS 同步段（反汇编还原的 argv）**：
```
/system/bin/ks -m -p /dev/efs_hsic_bridge -w /cpdump/ -t -1 -l -g <efs-prefix>
```
参考实现里同形态的命令行（Qualcomm kickstart 自带示例字符串）：
```
kickstart.exe -l -a 32 -m -v -w <dir>\ -p \\.\COM21 -t -1 -g m9k1_ > efs_sync_messages.txt
   ↑ "CALLING: kickstart.exe for EFS sync"
```
`<efs-prefix>` 是运行期从 pdata 里取的（mdm_helper 的 .rodata 里没有字面量，来自 `libmdmdetect`/私有数据；
mdm_helper 内部临时文件名用 `m9k0efs1.tmp`/`m9k0efs1bin.tmp`，所以前缀几乎肯定是 **`m9k0`**，参考文章用 `m9k1_`）。

---

## 2. 当前实验流程 vs 原厂流程（逐项差异）

`exp/run-load3.sh` 实际做的事：

```
1. mount /firmware, 建 /dev/block/modem/m9kefs*, stop ril-daemon
2. nohup esoc_reqeng     ← 打开 /dev/esoc-0, ESOC_REG_REQ_ENG(0xcc07), 常驻
3. nohup powerup /dev/subsys_esoc0  ← ioctl(0xCD01=SUBSYS_POWERUP) ✔ 正确
                                        (注：本内核 subsystem_get() 里的 subsys_start() 被 #if 0 掉了，
                                         subsystem_restart.c:519-527，所以必须用 ioctl，open 不够)
4. esoc_hsic             ← ioctl(/dev/esoc-0, 0xCC0C) ×3   ★★ 时机错误（见下）
5. ldrx                  ← Sahara over /dev/ks_hsic_bridge  ✔ 传输层没问题
6. 采样
```

| 原厂步骤 | 原厂时机 | 当前 | 后果 |
|---|---|---|---|
| `ESOC_REG_REQ_ENG` | 上电前 | ✔ | — |
| `ESOC_WAIT_FOR_REQ == ESOC_REQ_IMG` | 上电后 | ✔ | — |
| **HSIC unbind → NOTIFY(IMG_XFER_RETRY) → WAIT_FOR_REQ → 0.5s → bind → 等 `/dev/ks_hsic_bridge`（≤5 次）** | Sahara **之前**（`MODE_BOOT`） | ✘ **完全没做** | 属于「运气好也能跑」的项：当前流程是冷启动、EHCI 尚未枚举过，所以 Sahara 侥幸能通；但这一步同时是"清干净端口状态"的保险 |
| Sahara | — | ✔ 12/12 | — |
| `ESOC_NOTIFY(ESOC_IMG_XFER_DONE)` | **Sahara 之后** | ⚠ 发了但**时机错**：`esoc_reqeng.c:180` 在拿到 req engine 后立刻发（Sahara 之前） | 会提前武装 120s 的 `mdm2ap_status_check_work`；因为 120s ≫ 11.2s 且 STATUS 拉高时会 `cancel_delayed_work`，**本轮不影响结果**，但应改到 Sahara 之后 |
| `ESOC_GET_STATUS` 轮询到 STATUS=1 | — | 用 `/sys/kernel/debug/gpio` 轮询代替 | 等效 |
| **`configure_flashless_boot_dev(MODE_RUNTIME)`：unbind → 10ms → bind → `ESOC_SET_HSIC_READY`** | **STATUS 拉高之后** | ✘ **完全没做** | ★★★ 根因 |
| 等 `/dev/efs_hsic_bridge`（≤75 次）→ 起 EFS 同步 ks | STATUS 之后 | ✘ | ★★★ |
| `ESOC_NOTIFY(ESOC_BOOT_DONE)` | EFS 同步之后 | 部分（`esoc_reqeng` 的 `boot_done_watcher` 有，但发的时机是 STATUS 一拉高就发，早于 EFS 同步） | AP 侧只影响 `subsys0/state` |
| `ESOC_NOTIFY(ESOC_DIAG_DISABLE)`（`/dev/diag` 不存在时） | 最后 | ✘ | AP 侧 |

### 2.1 `AP2MDM_HSIC_READY` 的时机错误（独立的一条错误）

内核侧证据（`drivers/esoc/esoc-mdm-4x.c`）：

```c
 895:  if (gpio_is_valid(MDM_GPIO(mdm, AP2MDM_HSIC_READY))) { gpio_request(...); }
 903:  gpio_direction_output(MDM_GPIO(mdm, AP2MDM_HSIC_READY), 0);   // probe 时 = 0
 386:  case ESOC_PWR_ON:
 388:      gpio_set_value(MDM_GPIO(mdm, AP2MDM_HSIC_READY), 0);       // 上电时 = 0
 783:  static void mdm_set_hsic_ready(struct esoc_clink *esoc)
 786:      gpio_set_value(MDM_GPIO(mdm, AP2MDM_HSIC_READY), 0);
 787:      msleep(10);
 788:      gpio_set_value(MDM_GPIO(mdm, AP2MDM_HSIC_READY), 1);       // 唯一一次 0→1
```

`mdm_set_hsic_ready()` 只被 `ESOC_SET_HSIC_READY` ioctl 调用（`esoc_dev.c:241`），
即**内核刻意保证这个脚在上电后是低的，只由 userspace 在"AMSS 已就绪"那一刻打一个上升沿**。
`run-load3.sh` 在第 4 步（Sahara 之前）就连打 3 个脉冲，导致：

* modem 从 PBL 一路到 AMSS 期间该脚**恒为高**；
* AMSS 的 HSUSB 驱动起来时**看不到上升沿**。

firmware 侧证据（`apps.mbn`，build path 明写 `ZEROFLTE_CHN_CTC` —— 就是本机型的固件）：

```
hsu_gpio_hsic_ready_ctx                        ← HSIC-ready GPIO + 中断上下文
Assertion (dal_result == 0) && hsu_gpio_hsic_ready_ctx.h_tlmm failed
Assertion (dal_result == 0) && hsu_gpio_hsic_ready_ctx.h_gpio_int failed
SS_Hsic_host_ready_Raising_Signal_Isr          ← ★ 上升沿 ISR
SS_Hsic_host_ready_Falling_Signal_Isr
 - Clear and Stop Timer : is_host_ready_recvd=%d, is_hwgpio_timer_started=%d
 - Rearm Timer : is_host_ready_recvd=%d, is_hwgpio_timer_started=%d
 - Ignore because Host ready GPIO is Low
 - Ignore this function because Host ready GPIO is HGH
hsu_bam_sm__enumeration_with_ap                ← ★ "与 AP 枚举中" 状态
hsu_bam_sm_notify_host_ready
hsu_conf_sel_stack_utils_device_restart_core   ← 可以重启 USB core
hsu_err_fatal
sys_m_task() system:reset                      ← ★ system monitor 可以发起整机 reset
sys_m_task() ssr:retrieve:sfr                  ← 就是内核 sysmon_get_reason() 的那条命令
 - HSU LPM HSIC timed out.
```

> **注意**：`qdsp6sw.mbn`（56MB，DSP 固件）里**没有**这些串；HSU/HSIC/USB/MHI/sysmon 全在 `apps.mbn`（3.3MB）里。
> 所以 `apps.mbn` 才是"APPS 核（modem 的主 CPU）"的运行镜像，`MDM2AP_STATUS` 由它拉高：
> ```
> Set MDM2AP_STATUS_OUT_GPIO high
> MDM2AP_STATUS_OUT_GPIO go high
> ```
> 也就是说 `status = 1: mdm is now ready` **正是这条固件路径打印的对应 GPIO**，它紧接着就会进入等 host 的状态。

---

## 3. 根因假设（按可能性排序）

### H1（最可能，~70%）：缺失 `MODE_RUNTIME` 握手 → AMSS 阶段 USB 设备永不枚举 → modem 超时自复位

**机制**
1. AMSS 起来，`apps.mbn` 把 `MDM2AP_STATUS` 拉高（`101.203 status = 1`）。
2. AMSS 的 HSUSB 驱动初始化，试图以**复合设备**（PID 0x9048/0x904C/0x9075…）在 HSIC 上枚举。
3. AP 侧 EHCI 根端口上还挂着 PBL/SBL 阶段的 `05c6:9008` 设备 —— **没有任何 driver 去 unbind/rebind `s5p-ehci`，内核也收不到 disconnect**（`ks_bridge` 对 0x9008 还额外做了 `pm_runtime_forbid` on udev 和 udev->parent，`ks_bridge.c:954-957`）。
4. 于是 0x9048 从未出现 → `ks_bridge` 的 `efs_hsic_bridge` 不创建 → `hsic_sysmon` 不 probe → `diag_bridge` 不 probe → `sys_mon` platform device 不创建。
5. AMSS 的 `hsu_bam_sm__enumeration_with_ap` / `mhi_core_link_completion_timer_cb: Timeout` 之类的定时器到期 → `hsu_conf_sel_stack_utils_device_restart_core` 或 `sys_m_task() system:reset` → **受控**复位：`MDM2AP_STATUS` 拉低，但不报 `ERRFATAL`。

**支持证据**
* 原厂代码里这个分支**存在且带 50 次重试**，说明它是必需路径而不是可选优化。
* 公开的原厂 log（同族机型 trelte，见 §5）显示 unbind 时内核打印
  `s5p-ehci 15510000.usb: remove, state 1` / `usb 1-2: USB disconnect, device number 2` /
  `usb 1-2: unregistering interface 1-2:1.0` —— 这正是把 PBL 设备从总线上摘掉。
* 观测现象 100% 吻合：USB 恒为 9008、`/dev/efs_hsic_bridge` 从不出现、11.2s 稳定、ERRFATAL=lo。
* `mdm_hsic_pm` 的 `gpio_device_ready`(MDM2AP_DEVICERDY) 中断处理函数 `mdm_device_ready_irq_handler()` 只打日志什么也不做（`mdm_hsic_pm.c:989-998`）——所以 AP 侧**没有任何自动**重新枚举机制，只能靠 userspace。

**反证 / 需要验证的点**
* 如果 modem 的 HSIC PHY 在 SBL→AMSS 切换时确实做了一次真实的 disconnect/reconnect，EHCI 应该能自己看到。但从"USB 一直停在 9008"看，**要么没发生，要么发生了但 AP 没处理**。无论如何 unbind/bind 都能覆盖两种情况。

---

### H2（很可能，~50%，与 H1 并列/叠加）：`AP2MDM_HSIC_READY` 打早了，AMSS 收不到上升沿

见 §2.1。`SS_Hsic_host_ready_Raising_Signal_Isr` + `is_host_ready_recvd` + `is_hwgpio_timer_started`
说明 modem 是**靠边沿 + 定时器**判定的；电平早已为高时，要么 `is_host_ready_recvd` 永远为假而 timer 一直 rearm，
要么被 ` - Ignore this function because Host ready GPIO is HGH` 这条分支吞掉。

**支持证据**：内核刻意在 `ESOC_PWR_ON` 里把它清 0；原厂在 STATUS 之后才第一次拉高。
**反证**：modem 也可能只做电平判断（` - Ignore this function because Host ready GPIO is HGH` 暗示有电平分支），
那打早了就无害。**但这个假设的修复成本和 H1 是同一处代码，一起做即可。**

---

### H3（很可能，~50%，与 H1/H2 叠加）：没有 EFS 同步，AMSS 无法完成 boot

**机制**：这台机器的 modem **没有自己的 flash**（原厂 `ks` 的 `-s 16/17/20:/dev/block/modem/m9kefs{1,2,3}`
就是从 AP 的 UFS 分区读的）。运行期 modem 的 EFS 走 **HSIC EFS 桥**由 AP 代理读写。
`apps.mbn` 里存在 `EFS_SYNC/ACM/DIAG cur_mode=%d, port_id=%d, tx_ptr=%d`（`hsu_al_ser.c`），
说明 modem 侧有专门的 EFS_SYNC 模式；`ks.real` 里也有完整的 `-l / -g <prefix> / -t -1` EFS 同步循环。
AMSS 等不到 EFS 服务 → 判定 boot 失败。

**支持证据**：原厂流程里 EFS 同步是 `mdm9k_powerup` 的**最后一步且是强制的**（失败会 log 并返回错误）。
**反证**：也可能 modem 只用 Sahara 阶段下发的 efs1/2/3 镜像就够了，EFS 同步是"AP 侧想读 modem 的 EFS"
（比如取 IMEI/校准）而不是 modem 求 AP。**需要 H1 修好后看 `/dev/efs_hsic_bridge` 是否出现才能分辨。**

---

### H4（中等，~25%）：双链路（HSIC + PCIe）里 PCIe/MHI 没起来

**证据**
* `esoc-mdm-4x.c:1103` **硬编码** `esoc->link_name = MDM9x35_DUAL_LINK;`（"HSIC+PCIe"）。
* `drivers/mhi/mhi_ssr.c:24-35`：**只有当 link == "HSIC+PCIe" 时才走 `STATE_TRANSITION_RESET`**，
  否则 `STATE_TRANSITION_BHI`（意味着 MHI 要自己下发固件）。
* 原厂 `stock/init.baseband.rc`（本机原厂脚本）明确用了 MHI：
  `write /sys/module/mhi_uci/parameters/mhi_uci_dump 2`、`/sys/class/net/rmnet_mhi0/...`、
  `/sys/bus/pci/drivers/mhi/0000:01:00.0/MHI_MOBILE_HOTSPOT`。
* 原厂 DTB 与本地编译 DTB 都有 `pcie0@155C0000 { status = "okay" }` 和 `qcom,mhi` 节点。
* `apps.mbn` 里有 `mhi_core_link_completion_timer_cb: Timeout`、`pcie_phy_init`、
  `- Wait till enumerate completes by Root Complex(AP host)`、`PCIE_LINKSTATUS_ENUMERATED`。

**反证**：boot/EFS 阶段走的是纯 HSIC（`mdm_helper` 只认 "HSIC" 前缀），MHI 是给运行期 QMI/rmnet 用的；
modem 不一定会在 boot 阶段等 PCIe。而且仓库里 `boot-F/I/J-eur-mdm-nopcie.img` 是把 PCIe 关掉的，
需要确认**当前刷的 `boot-AD-reqleft.img` 用的 DTB 里 PCIe 是 okay**。
（已核对：`/home/duanjb666/kbuild/mdm-out/arch/arm64/boot/dts/exynos7420-zeroflte_chn_0*.dtb`
反编译后 `pcie0@155C0000 status = "okay"`、`qcom,mdm1`/`mdmpm_pdata`/`qcom,mhi` 都在 —— DTB 没问题。）

---

### H5（低，~10%）：`mdm_hsic_pm` 没有 probe 成功

`mdm_status_fn()`（`esoc-mdm-4x.c:508-521`）在 STATUS 上升沿会调用
`request_active_lock_set("15510000.mdmpm_pdata")`，最终落到 `get_pm_data_by_dev_name()`。
若 `mdm_hsic_pm` 没 probe 成功，`pm_data == NULL` → 只少一个 wake_lock，**不会导致 modem 复位**，
但会让 `pm_dev_runtime_get_enabled()` / `mdm_hsic_irq_handler`（host wake，MDM2AP_HOSTWAKE gpa3-1）
这条链路失效。需要确认 `/sys/bus/platform/drivers/mdm_hsic_pm/15510000.mdmpm_pdata` 存在。

---

### H6（低，~5%，且与现象不符）：`U5_TEMP` 把 `mdm_update_gpio_configs()` 整个编译掉了

`esoc-mdm-4x.c:99,259` 用 `#if defined(U5_TEMP)` 包住整个 `mdm_update_gpio_configs()`，
`U5_TEMP` 在本仓库（含所有 defconfig）**都没有定义** → 函数是空函数，
`GPIO_UPDATE_RUNNING_CONFIG` 什么都不做。

**判断：这不是根因。** 它只是"AMSS 起来后把 MDM2AP_STATUS 脚的 pinctrl 从 booting 配置切成 running 配置"
（拉/驱动强度微调），改不改都不影响 modem 自己拉低 STATUS。但值得记一笔：
`zerolte_modem-mdm9x35.dtsi` 里 `mdm2ap_status_gpio_run_cfg` 也根本没被赋值，所以这段代码
在本平台**本来就是死代码**。**不建议为此改内核。**

---

## 4. 「11 秒」是什么

**先排除 AP 侧**（我逐个核过 kernel 里所有相关超时）：

| 位置 | 值 | 触发 |
|---|---|---|
| `esoc-mdm-drv.c:176` `wait_for_completion_timeout(&boot_done)` | 60000ms（曾被改成 300s→900s） | 只影响 `mdm_subsys_powerup()` 返回，**不会**动 STATUS |
| `esoc-mdm-4x.c:42` `MDM2AP_STATUS_TIMEOUT_MS` | **120000ms** | `mdm_notify(ESOC_IMG_XFER_DONE)` 后 120s 内 STATUS 没拉高 → `ESOC_UNEXPECTED_RESET` |
| `esoc-mdm-4x.c:43` `MDM_MODEM_TIMEOUT` | 3000ms | 只在 `ESOC_PRIMARY_CRASH` 里等 STATUS 变低 |
| `esoc-mdm-4x.c:406` PWR_OFF 等 STATUS 变低 | 10000ms | 关机路径 |
| `hsic_sysmon.c` 超时 | `HSIC_SYSMON_TIMEOUT`（数百 ms 级） | sysmon 读写 |
| `mdm_hsic_pm.c` LPA/RPM | 20ms~200ms | 电源态 |

**→ AP 侧没有任何 ~11s 的定时器。** 而且 `mdm_status_change()`（`esoc-mdm-4x.c:704-706`）
里 `value==0 && mdm->ready` 才打印，`mdm->ready` 只在 value==1 时置位 —— 所以
`unexpected reset external modem` **一定是 modem 自己把 STATUS 拉低的**（与简报一致）。

> ⚠ **需要纠正一处旧注释**：`tools/esoc_reqeng.c:12` 写着
> 「evt fifo 只有 4 格且无人读 → 塞满后 …… modem 在 STATUS 拉高约 11s 后被判 `ESOC_UNEXPECTED_RESET` 复位」。
> 这个因果链**在物理上不成立**：evt fifo 塞满只会让 `esoc_clink_evt_notify()` 丢事件/报错，
> **任何 AP 侧代码都不会去拉低 `MDM2AP_STATUS` 这个输入脚**。
> 而且简报 §3.3 已说明 fifo 抽干后该报错归零、§3.4 说明 v40 移除自复位后 11.2s 依旧。
> 请把 11.2s 归因到 **modem 固件内部定时器**，不要再往 evt fifo 方向排查。

**modem 侧**：`apps.mbn` 里有明确的"等 host"状态机与定时器：
```
hsu_bam_sm__enumeration_with_ap          与 AP 枚举中
hsu_bam_sm_notify_host_ready
SS_Hsic_host_ready_{Raising,Falling}_Signal_Isr
is_host_ready_recvd / is_hwgpio_timer_started  +  Rearm Timer / Clear and Stop Timer
hsu_bam_sm__data_activity_while_force_shutdown
hsu_conf_sel_stack_utils_device_restart_core     可重启 core
mhi_core_link_completion_timer_cb: Timeout        MHI 链路完成定时器
sys_m_task() system:reset                        可发起 system reset
 - HSU LPM HSIC timed out.
AP didn't response about the remote wakeup within 1sec    ← 说明这类握手超时是 1s 量级可配的
```
`hsu_gpio_hsic_ready_ctx` / `hsu_apq2mdm_gpio_intr_ctx` 的 `h_tlmm`/`h_gpio_int` 断言说明
这两个 GPIO（= `AP2MDM_HSIC_READY` 与某个 `APQ2MDM_*`）是**带中断注册**的 —— 边沿语义，不是纯电平。

**结论**：11.2s 是 modem 固件内部"等 AP 完成 HSIC/枚举/host-ready 握手"的定时器。
它稳定（11.19/11.18/11.20）正说明是一个固定常量而不是竞争。

**怎么拿到确切原因（SFR）**：modem 支持通过 sysmon 通道回 SFR ——
`arch/arm64/mach-exynos/sysmon.c:276-305` 的 `sysmon_get_reason()` 发 `"ssr:retrieve:sfr"`，
期望收 `"ssr:return:<reason>"`。但是：
* 该通道是 **HSIC 上的 sysmon USB 接口**（`hsic_sysmon.c:418-430`：PID 0x9048/… **interface 1**），
  所以**必须先让 AMSS 阶段设备枚举成功**，才能读到 SFR。
* 内核只在 `ESOC_EXIT_DEBUG` 里把 `mdm->get_restart_reason = true`（`esoc-mdm-4x.c:475`），
  普通 unexpected reset 不会去查 SFR。

→ **SFR 只能作为 H1 修好之后的"确认 + 后续排错"手段，不能作为当前的诊断手段。**
（换句话说：读不到 SFR 这件事本身就是"AMSS 阶段 USB 从未枚举"的又一个证据。）

---

## 5. 上游/原厂现成实现对照（这是最大的收获）

### 5.1 同族机型的完整原厂实现文档（含真实 log）

* **《MDM9x25 Flashless boot & IPC over HSIC》** —— 原始出处 CSDN：
  <https://blog.csdn.net/hongzg1982/article/details/54884883>
  镜像：<https://www.vevb.com/wen/2019/11-10/149752.html>、<https://www.vevb.com/wen/2019/11-10/151017.html>
* 这篇文章的 log 里设备主机名是 **`root@royceltectc`**（Galaxy S6 edge+ 中国电信版 `royceltectc`），
  平台是 **Exynos + MDM9x25/9x35 + HSIC**，与我们这台 `zerofltectc` 是同一家族、同一套
  `mdm_helper` / `ks` / `ks_bridge` / `esoc` 代码。**它就是我们的"正确流程参考实现"。**
* 文章给出了 `configure_flashless_boot_dev()` 的**完整 C 源码**，与我反汇编出来的
  分支结构、ioctl 顺序、延迟值（`usleep(500000)` / `usleep(10000)`）**逐条一致**。
* 文章给出了 `mdm_hsic_peripheral_cmd()` 的源码（就是上面 §1.3 的 bind/unbind）。
* 文章给出了 unbind 时的真实内核 log：
  ```
  [ALOG] MDM9x35: configure_flashless_boot_dev: Initiating HSIC unbind
  s5p-ehci 15510000.usb: remove, state 1
  s5p-ehci 15510000.usb: roothub graceful disconnect
  usb usb1: USB disconnect, device number 1
  usb 1-2: USB disconnect, device number 2
  usb 1-2: unregistering device
  usb 1-2: unregistering interface 1-2:1.0
  ...
  [ALOG] MDM9x35: configure_flashless_boot_dev: Initiating HSIC bind
  s5p-ehci 15510000.usb: s5p_ehci_probe
  s5p-ehci 15510000.usb: new USB bus registered, assigned bus number 1
  ...
  usb 1-2: New USB device found, idVendor=05c6, idProduct=9008
  usb 1-2: Product: QHSUSB__BULK
  ks_bridge 1-2:1.0: usb_probe_interface
  ```
  → **bind 之后 modem 才会重新出现在总线上。**

### 5.2 上游内核代码

* CodeAurora `drivers/esoc/esoc-mdm-4x.c`（msm-3.10）：
  <https://android.git.googlesource.com/kernel/msm/+/bd91122223fd4cf1f5bbda7d03b488f4292c38a3/drivers/esoc/esoc-mdm-4x.c>
  → 与本机 `drivers/esoc/esoc-mdm-4x.c` 基本同源（本机是 Samsung 魔改版：
  多了 `AP2MDM_HSIC_READY`、`ESOC_SET_HSIC_READY`、`ESOC_GET_RESTART_REASON`、
  `set_ap2mdm_errfatal`、`mdm_get_fatal_status/mdm_get_modem_status`、`U5_TEMP` 包裹的 gpio config）。
  **上游版没有 HSIC_READY 这条路径** —— 这是 Samsung 为"外挂 modem + HSIC"加的，
  必须由 userspace 驱动，所以**上游没有现成的 "STATUS 之后做什么" 实现可抄**，
  可抄的是 Samsung 的 `mdm_helper`。
* mdm_helper 的 SELinux 策略（LineageOS 移植参考）：
  <https://git.replicant.us/mirrors/LineageOS/android_device_qcom_sepolicy/plain/common/mdm_helper.te>
  <https://git.replicant.us/mirrors/LineageOS/android_device_qcom_sepolicy-legacy/tree/common/mdm_helper.te>
* `mdm_common.c`（另一套 Samsung Exynos MDM 实现，可对照 `reset_mdm_cb`）：
  <https://raw.githubusercontent.com/neobuddy89/NX-Kernel/refs/heads/master/arch/arm/mach-exynos/mdm_common.c>
* 本机内核里就是参考实现的同源代码（**不需要去网上找，本地就有**）：
  * `drivers/usb/host/ehci-s5p.c:835-845` → `platform_driver s5p_ehci_driver { .driver.name = "s5p-ehci" }`，
    没有 `suppress_bind_attrs` → **sysfs 的 `bind`/`unbind` 可用**。
  * `drivers/usb/misc/ks_bridge.c:67-68`：`EFS_HSIC_BRIDGE_INDEX=2`、`EFS_USB_BRIDGE_INDEX=3`；
    `:580-610` PID 表；`:801-960` probe（`/dev/efs_hsic_bridge` 在 probe 里 `device_create`，
    **所以设备不枚举就绝不会有这个节点**）。
  * `arch/arm64/mach-exynos/hsic_sysmon.c:418-430`：sysmon 通道 = 同一 PID 的 **interface 1**。
  * `drivers/usb/misc/diag_bridge.c:592,596`：diag = 0x9048/0x9075 的 **interface 0**。
  * `drivers/usb/misc/mdm_data_bridge.c:1117-1132`：rmnet 数据 = 0x9048/0x904C/0x9075 的
    **interface 3/4/5**。
  * `drivers/mhi/mhi_ssr.c:24-35`：link 必须等于 `"HSIC+PCIe"` 才走 `STATE_TRANSITION_RESET`。

### 5.3 关键差异点（本机 vs 参考实现）

| 项 | 参考实现（trelte / roycelte） | 本机 G9209 |
|---|---|---|
| EHCI | `15510000.usb` @ `s5p-ehci` | **完全相同** |
| unbind/bind 节点 | `/sys/bus/platform/drivers/s5p-ehci/{unbind,bind}`，写入 `15510000.usb` | 完全相同（`ehci-s5p.c:840 .name="s5p-ehci"`） |
| `/dev/subsys_esoc0` 触发上电 | `subsys_device_open()` 直接 `subsys_start()` | **本内核把 `subsystem_get()` 里的 `subsys_start()` `#if 0` 掉了**（`subsystem_restart.c:519-527`）→ 必须用 `ioctl 0xCD01`，仓库里的 `powerup` 工具已经是对的 |
| 运行时 IPC | HSIC（`rmnet_usb`, `hsicctl0`） | **PCIe + MHI**（`rmnet_mhi0`, `mhi_uci`, `0000:01:00.0`）+ HSIC 用于 boot/EFS/ramdump |

---

## 6. 可执行的验证 / 修复动作

> 原则：**先只改 userspace/脚本，不动内核**。所有动作都能在重启后的干净状态下重放。

### 动作 0（零成本、最先做）：打点确认「AMSS 阶段 USB 从未枚举」

在 `run-load3.sh` 的采样里补上这些判据（现在只看了 `/sys/bus/usb/devices/*/idProduct`）：

```sh
# 1) AMSS 阶段复合设备是否出现
for d in /sys/bus/usb/devices/*/; do
    v=$(cat $d/idVendor 2>/dev/null); p=$(cat $d/idProduct 2>/dev/null)
    [ "$v" = "05c6" ] && echo "$d $v:$p"
done
# 2) 各桥是否 probe（节点是 probe 时创建的，不存在就是没枚举）
ls -l /dev/ks_hsic_bridge /dev/efs_hsic_bridge /dev/diag_bridge /dev/mdm* 2>&1
# 3) sysmon（SFR 通道）是否起来 —— 它绑定 interface 1
ls -l /sys/bus/platform/drivers/sys_mon/ ; ls -l /sys/bus/platform/devices/ | grep -i sys_mon
# 4) mdm_hsic_pm 是否 probe 成功
ls -l /sys/bus/platform/drivers/mdm_hsic_pm/
ls -l /sys/bus/platform/devices/15510000.mdmpm_pdata 2>&1
# 5) ESOC 是否在线
cat /sys/bus/msm_subsys/devices/subsys0/state
cat /sys/bus/esoc/devices/esoc0/{esoc_name,esoc_link,esoc-dev}
# 6) dmesg 关注点
dmesg | grep -E "s5p-ehci|s5p_ehci_probe|USB disconnect|ks_bridge|hsic_sysmon|sys_mon|qcom,mdm1|ext-mdm|mdmpm|esoc|mhi|pcie"
```

**预期（当前）**：`idProduct` 只有 9008；`/dev/efs_hsic_bridge` 不存在；`sys_mon` 没有；
`subsys0/state` 是 `OFFLINE`；`dmesg` 里**没有** `s5p-ehci 15510000.usb: remove, state 1`。

### 动作 1（★核心修复）：在 STATUS 拉高后执行原厂的 `MODE_RUNTIME` 序列

把这 5 行插到「检测到 `MDM2AP_STATUS=hi`」之后、发 `ESOC_BOOT_DONE` 之前
（`esoc_reqeng.c` 的 `boot_done_watcher()` 里，或者独立写成 `tools/mdm_runtime_handshake.c`）：

```sh
EHCI=/sys/bus/platform/drivers/s5p-ehci
# A) 强制重新枚举 AMSS 阶段设备
echo 15510000.usb > $EHCI/unbind
sleep 0.01                      # 原厂 usleep(10000)
echo 15510000.usb > $EHCI/bind
sleep 0.3                       # 给 khubd 一点时间
# B) 打 AP2MDM_HSIC_READY 的上升沿（必须在这一刻，不能更早）
#    等价 ioctl(/dev/esoc-0, ESOC_SET_HSIC_READY = 0xCC0C)
$M/esoc_hsic
# C) 等 EFS 桥
for i in $(seq 1 75); do [ -e /dev/efs_hsic_bridge ] && break; sleep 0.1; done
# D) 跑 EFS 同步（前台或后台）
[ -e /dev/efs_hsic_bridge ] && \
  /system/bin/ks -m -p /dev/efs_hsic_bridge -w /cpdump/ -t -1 -l -g m9k0 &
# E) 告诉内核 boot 完成
#    ioctl(/dev/esoc-0, ESOC_NOTIFY = 0x4004CC03, &{2 /*ESOC_BOOT_DONE*/})
```

**并且把 `run-load3.sh` 第 3 步的 `$M/esoc_hsic` 删掉**（不要提前打脉冲）。

配套的 C 版本（推荐，避免 shell 时序抖动）——`tools/mdm_runtime_handshake.c`：
```c
/* 伪码：在 STATUS=hi 后执行 */
wr("/sys/bus/platform/drivers/s5p-ehci/unbind", "15510000.usb");
usleep(10000);
wr("/sys/bus/platform/drivers/s5p-ehci/bind",   "15510000.usb");
usleep(300000);
ioctl(esoc_fd, ESOC_SET_HSIC_READY /*0xCC0C*/);
wait_for("/dev/efs_hsic_bridge", 75, 100 /*ms*/);
if (fork()==0) execl("/system/bin/ks","ks","-m","-p","/dev/efs_hsic_bridge",
                     "-w","/cpdump/","-t","-1","-l","-g","m9k0",NULL);
{
  unsigned n = ESOC_BOOT_DONE /*2*/;
  ioctl(esoc_fd, ESOC_NOTIFY /*0x4004CC03*/, &n);
}
if (access("/dev/diag", F_OK)) { unsigned n = 12; ioctl(esoc_fd, ESOC_NOTIFY, &n); }
```

**同时把 `esoc-mdm-drv.c:176` 的 900000ms 改回 60000ms** —— 一旦 `ESOC_BOOT_DONE` 正常送达，
`boot_done` 会立刻 complete，放宽超时只会掩盖真正的失败。
（如果担心调试期，先保留 900s 也行，但要明白它是症状补丁。）

**安全提示**：`15510000.usb` 是 HSIC EHCI（接 modem）；
设备自己的 USB/ADB 走 `usb@15400000` 的 **DWC3**（`exynos7420-zero_common.dtsi:845`），
`ks_bridge.c:45-52` 的 `str_to_busid()` 也只认 `15510000.usb`(=HSIC) 和 `msm_ehci_host.0`。
所以 unbind/bind `s5p-ehci` **理论上不会掉 adb**。但两者共用 FSYS0 的 USB2 PHY 电源域
（`usb2phy@15530000` / `usbphy-sys` @`0x105c0700`），
**第一次试验务必用 `nohup setsid` 后台跑 + 串口/本地终端兜底**（简报 §6 的规矩 6）。

### 动作 2：验证 H2 是否独立成立

在动作 1 成功之前，可以做一个**最小对照实验**：
只做 §动作1 的 A+B 两步（不跑 EFS 同步），看 `05c6:90xx` 是否出现、STATUS 是否不再 11s 掉。
* 若 A+B 就解决了 → 根因是 H1(+H2)，EFS 同步只是运行期需要。
* 若 A+B 后能看到 90xx 但 STATUS 仍在 11s 掉 → 根因含 H3（必须跑 EFS 同步）。

### 动作 3：确认 PCIe/MHI（H4）

```sh
dmesg | grep -iE "pcie|exynos-pcie|mhi|link up|LTSSM"
cat /sys/bus/pci/devices/0000:01:00.0/vendor 2>/dev/null
ls /sys/bus/pci/drivers/mhi/
```
若 PCIe 从未 link up：
* 确认 `pcie0@155C0000 { status="okay" }`（已确认当前 DTB 是 okay）；
* 确认 `qcom,mdm-dual-link` / `mdm-link-detect` —— 本 DTS **没有**这两个属性，
  但 `mdm9x35_setup_hw()` 无条件强制成 `"HSIC+PCIe"`，所以 esoc 侧没问题；
* 必要时对照 `arch/arm64/boot/dts/exynos7420-zeroflte_eur_open_06_mdm_nohsic.dts`
  （Samsung 自己提供的"无 HSIC"版本，可反推它对 PCIe 的假设）。

### 动作 4：修完 H1 后读 SFR，拿到 modem 的确切复位原因

两条路（都要在能枚举出 90xx 之后才有意义）：
1. **不改内核**：让 `mdm_helper` 的等价逻辑在 STATUS 上升沿先 `ESOC_NOTIFY(ESOC_DIAG_DISABLE)` 之类……
   不行 —— 内核只在 `ESOC_EXIT_DEBUG` 里置 `get_restart_reason`。
2. **一行内核改动**（低风险）：在 `mdm_status_change()` 的 `value == 1` 分支里无条件
   `queue_work(mdm->mdm_queue, &mdm->restart_reason_work);`（去掉 `if (mdm->get_restart_reason)` 条件），
   这样 modm 每次重新拉起 STATUS 都会把上一次的 SFR 打出来：
   `dev_err(dev, "mdm restart reason is %s\n", sfr_buf)`（`esoc-mdm-4x.c:535`）。
   *前提*：sysmon HSIC 通道（interface 1）已经枚举成功 —— 所以这是**修好之后**的排错工具。

### 动作 5：如果修好 H1 后 RILD 侧还要活起来（后续工作，不在本次范围）

原厂 `stock/init.baseband.rc` 已经在仓库里，直接照抄服务定义即可：
```
service mdm_helper /system/bin/mdm_helper
    class core
service mdm_helper_proxy /system/bin/mdm_helper_proxy
    class core
    disabled
on property:init.svc.ril-daemon=running
    setprop ro.mdm_helper_proxy_req true
on property:ro.mdm_helper_proxy_req=true
    start mdm_helper_proxy
```
配上 `port-blobs/` 里已经备齐的 `vendor/bin/{qmuxd,rild,irsc_util,diag_*}`、
`vendor/lib64/libqmi*.so`、`libmdmdetect.so`，加上 §5.2 的 `mdm_helper.te` sepolicy。
**更简单的一条路**：直接把我们自己版本的 `ks`（`evidence/ks.real` 就是 Qualcomm kickstart）
和 `mdm_helper` 塞回去用 —— 它们本来就是这个机型的原厂二进制。

---

## 7. 关键源码片段汇总（本机，带行号）

| 文件:行 | 内容 |
|---|---|
| `drivers/esoc/esoc-mdm-4x.c:99,256-288` | `#if defined(U5_TEMP)` 把 `mdm_update_gpio_configs()` 整个包住；`U5_TEMP` 全仓库未定义 → **空函数**（H6，非根因） |
| `drivers/esoc/esoc-mdm-4x.c:386-392` | `ESOC_PWR_ON`: `AP2MDM_HSIC_READY = 0` |
| `drivers/esoc/esoc-mdm-4x.c:508-521` | `mdm_status_fn()`：STATUS 上升沿的 work；只做 `request_active_lock_set()` + 空函数 |
| `drivers/esoc/esoc-mdm-4x.c:546-565` | `mdm_notify()`：`ESOC_IMG_XFER_DONE` / `ESOC_BOOT_DONE→ESOC_RUN_STATE` |
| `drivers/esoc/esoc-mdm-4x.c:692-716` | `mdm_status_change()`：`unexpected reset external modem` 出处 |
| `drivers/esoc/esoc-mdm-4x.c:783-789` | `mdm_set_hsic_ready()`：唯一的 `AP2MDM_HSIC_READY` 0→10ms→1 脉冲 |
| `drivers/esoc/esoc-mdm-4x.c:895-903` | `AP2MDM_HSIC_READY` gpio_request + 初始化为 0 |
| `drivers/esoc/esoc-mdm-4x.c:1051-1127` | `mdm9x35_setup_hw()`；`:1103` 硬编码 `link_name="HSIC+PCIe"` |
| `drivers/esoc/esoc-mdm-drv.c:141-187` | `mdm_subsys_powerup()`：`wait_for_completion_timeout(&boot_done)`（已被改成 900s） |
| `drivers/esoc/esoc-mdm-drv.c:56-83` | `mdm_handle_clink_evt()`：`ESOC_RUN_STATE → complete(&boot_done)` |
| `drivers/esoc/esoc_dev.c:241` | `ESOC_SET_HSIC_READY → clink_ops->set_hsic_ready()`（唯一入口，纯 userspace 驱动） |
| `drivers/usb/misc/ks_bridge.c:45-52` | `str_to_busid()`：HSIC = `15510000.usb` |
| `drivers/usb/misc/ks_bridge.c:580-610` | PID 表：0x9008/if0 → `ks_hsic_bridge`；0x9048|904C|9075|…/if2 → `efs_hsic_bridge` |
| `drivers/usb/misc/ks_bridge.c:918-942` | `alloc_chrdev_region/class_create/cdev_add/device_create(mdev->name)` → **节点在 probe 时才创建** |
| `drivers/usb/misc/ks_bridge.c:954-957` | 0x9008 时 `pm_runtime_forbid(udev->dev.parent)` + `pm_runtime_forbid(udev->dev)` |
| `drivers/usb/host/ehci-s5p.c:835-845` | `platform_driver s5p_ehci_driver { .driver.name = "s5p-ehci" }`（无 `suppress_bind_attrs`） |
| `drivers/usb/host/ehci-s5p.c:171-200` | `phy_register_notifier` / `usb_phy_prepare_shutdown/wakeup`（HSIC LPA） |
| `arch/arm64/mach-exynos/hsic_sysmon.c:418-430` | sysmon（SFR）USB 接口 = 0x9048/… **interface 1** |
| `arch/arm64/mach-exynos/sysmon.c:49-54,276-305` | `SYSMON_SS_EXT_MODEM` 走 `TRANSPORT_HSIC`；`sysmon_get_reason()` 发 `"ssr:retrieve:sfr"` |
| `arch/arm64/mach-exynos/subsystem_restart.c:874-891` | `SUBSYS_POWERUP` ioctl = `_IO(0xCD,1)` = `0xCD01` |
| `arch/arm64/mach-exynos/subsystem_restart.c:519-527` | `subsystem_get()` 里的 `subsys_start()` 被 `#if 0` 掉 → **open 不够，必须 ioctl** |
| `drivers/mhi/mhi_ssr.c:24-35` | esoc link == `"HSIC+PCIe"` → `STATE_TRANSITION_RESET`，否则 `STATE_TRANSITION_BHI` |
| `include/uapi/linux/esoc_ctrl.h` | ioctl 编码：`ESOC_CMD_EXE=0x4004CC01`、`ESOC_WAIT_FOR_REQ=0x8004CC02`、`ESOC_NOTIFY=0x4004CC03`、`ESOC_GET_STATUS=0x8004CC04`、`ESOC_REG_REQ_ENG=0xCC07`、`ESOC_SET_HSIC_READY=0xCC0C`；`ESOC_REQ_IMG=1`、`ESOC_BOOT_DONE=2`、`ESOC_BOOT_FAIL=3`、`ESOC_IMG_XFER_RETRY=4` |
| `include/uapi/linux/pci_regs.h`… | （无关） |

### 固件侧字符串（`stock-cp/image/apps.mbn`）
```
/home/dpi/qb5_8814/workspace/ZEROFLTE_CHN_CTC/apps_proc/core/wiredconnectivity/hsusb/...   ← 就是本机型
Set MDM2AP_STATUS_OUT_GPIO high / MDM2AP_STATUS_OUT_GPIO go high
hsu_gpio_hsic_ready_ctx / Assertion ... hsu_gpio_hsic_ready_ctx.h_gpio_int failed
hsu_apq2mdm_gpio_intr_ctx
SS_Hsic_host_ready_Raising_Signal_Isr / SS_Hsic_host_ready_Falling_Signal_Isr
 - Rearm Timer : is_host_ready_recvd=%d, is_hwgpio_timer_started=%d
 - Clear and Stop Timer : is_host_ready_recvd=%d, is_hwgpio_timer_started=%d
 - Ignore because Host ready GPIO is Low /  - Ignore this function because Host ready GPIO is HGH
hsu_bam_sm__enumeration_with_ap / hsu_bam_sm_notify_host_ready
hsu_conf_sel_stack_utils_device_restart_core / hsu_err_fatal
mhi_core_link_completion_timer_cb: Timeout
sys_m_task() system:reset / ssr:poweroff / ssr:retrieve:sfr / SYS_M_SYSTEM_DIAG_DISABLED
- Ignore the SYS_M_AP2MDM_STATUS_GPIO low interrupt because it's not expected level.
 - HSU LPM HSIC timed out.
AP didn't response about the remote wakeup within 1sec
```

### mdm_helper 里的地址→字符串对照（便于复核）
```
0x48ca "/system/bin/ks"        0x48d9 "-m"     0x48dc "-p"    0x48df "-w"
0x48e2 "-t"                    0x48e5 "-1"     0x48e8 "-l"    0x48eb "-g"
0x4608 ".../s5p-ehci/bind"     0x4630 ".../s5p-ehci/unbind"   0x465a "15510000.usb"
0x45df "/dev/ks_hsic_bridge"   0x45f3 "/dev/efs_hsic_bridge"
0x4adc "%s: MDM2AP_STATUS is now high"
0x51ed "%s: Setting up %s boot link"
0x5249 "%s: %s: Initiating HSIC bind"
0x4ce0 "%s: %s: Initiating HSIC unbind"
0x5266 "%s: %s: Failed to setup HSIC link"
0x5288 "%s: Setting up %s link for efs_sync"
0x52ac "%s: Sending boot status notification to HSIC"
0x52d9 "%s: %s:hsic_ready failed"
0x4b36 "%s: Could not detect EFS sync port"
0x4bab "%s: Failed to exec KS process for efs sync"
0x4bd6 "%s: Failed to send ESOC_BOOT_DONE notification"
0x4c0f "%s: Diag node is not exist. fd %d"
```

---

## 8. 一句话回答题目

> **STATUS 拉高只代表 "modem 的 APPS 核跑起来了、在等 AP"。AP 必须紧接着做四件事：
> ① unbind/rebind `s5p-ehci`（`15510000.usb`）逼 HSIC 重新枚举出 AMSS 阶段的 `05c6:90xx` 复合设备；
> ② 这时才第一次把 `AP2MDM_HSIC_READY` 拉高（`ESOC_SET_HSIC_READY`），给 modem 一个上升沿；
> ③ 等 `/dev/efs_hsic_bridge` 出现并用 `ks -l -g <prefix>` 给它做 EFS 同步（modem 没有 flash，EFS 在 AP 的 m9kefs 上）；
> ④ 最后 `ESOC_NOTIFY(ESOC_BOOT_DONE)`。
> 少做 ①②，modem 就在自己的 host-ready/枚举定时器（≈11.2 s）到期后受控地拉低 STATUS 自复位——
> 所以 ERRFATAL 一直是 lo，也永远看不到 0x9048 和 `/dev/efs_hsic_bridge`。**

**验证顺序建议**：动作 0（打点）→ 动作 1 的 A+B（最小对照）→ 动作 1 全量 → 动作 4（SFR）。
