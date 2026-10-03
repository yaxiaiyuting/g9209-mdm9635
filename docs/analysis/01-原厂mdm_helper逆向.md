# 原厂 `mdm_helper` 启动时序完整逆向（SM-G9209 / MDM9635）

> 纯静态逆向 + 离线分析。**未使用 adb、未接触真机、未修改 `baseband-study/` 下任何已有文件。**
> 本文件是新增报告。
>
> 分析对象：`baseband-study/stock/qcom-bin/mdm_helper`
> md5 `6d2bbae972ccbc90f2c23bb3219b5405`，27272 B，ELF64 aarch64 PIE，stripped，
> interpreter `/system/bin/linker64`，gold 1.11 链接，`.text` = `0x1360..0x3ef0`（11152 B）。
> 三份副本 md5 完全一致：`stock/qcom-bin/`、`port-blobs/vendor/bin/`、`portmod/system/vendor/bin/`。
>
> 生成时间：本轮分析（2026-10-03）

---

## 0. 结论速览（给赶时间的人）

| # | 结论 | 证据强度 |
|---|---|---|
| 1 | **`peripheral_reset()` 就是「写 unbind → `usleep(10000)` → 写 bind」，没有任何 PHY/PMU 寄存器操作，也没有别的 sysfs 节点。** 唯一被打开的两个路径是 `/sys/bus/platform/drivers/s5p-ehci/unbind` 与 `.../bind`，内容都是 `"15510000.usb"`。 | ★★★★★ 反汇编 + 原厂 C 源码 + 原厂内核 log 三方互证 |
| 2 | **`mdm_helper` 里根本没有 GPIO / PHY / devicerdy / hsicctl 相关字符串**（整张符号串表共 187 条 rodata 字符串 + 60 条 dynstr 已逐条枚举）。它**不读 GPIO、不等 `MDM2AP_DEVICERDY`、不碰 `mdm2ap-hostwake-gpio`**。所有 GPIO 等待都在**内核**里（`mdm_pblrdy_change` / `mdm_status_change` / `mdm_set_hsic_ready`）。 | ★★★★★ 全量字符串枚举 |
| 3 | **`ESOC_SET_HSIC_READY`(0xCC0C) 之后 `configure_flashless_boot_dev` 立刻返回**。下一个动作在调用者 `mdm9k_powerup()` 里：`WaitForCOMport("/dev/efs_hsic_bridge", 75, 0)`。 | ★★★★★ |
| 4 | **`WaitForCOMport` 的单位是 0.5 秒**：`timeout=75` ⇒ 最多 **37.5 秒**（不是 75 秒）。`timeout=5` ⇒ 2.5 s，`timeout=10` ⇒ 5 s。失败返回 1，**本身不重试**（重试由调用者的 50 次循环负责）。 | ★★★★★ |
| 5 | **原厂 `MODE_RUNTIME` 期间会先解绑再绑定 EHCI，然后才拉 HSIC_READY；并且在这之前，`MODE_BOOT` 阶段已经做过一次「unbind → 软复位 modem → 等 PBL → 500ms → bind」**。我们现在这一整段是缺的（详见 §4）。 | ★★★★★ |
| 6 | **modem ready 之后 AP 侧的 `mdm_helper` 什么都不做，只阻塞在 `ESOC_WAIT_FOR_REQ`**（内核 `wait_event_interruptible`）。没有轮询、没有定时器、没有保活。 | ★★★★★ |
| 7 | AMSS（`stock-cp/image/apps.mbn`）里带 `ZEROFLTE_CHN_CTC` 构建路径，其 `hsu_al_task.c` 有 `SS_Hsic_host_ready_Raising_Signal_Isr` + `is_host_ready_recvd` + `Rearm Timer`，以及 `" - HSIC DEVICE_READY Setting HIGH, USB PHY ON"` / `" - make the HSIC connect signal"`。**即 AMSS 把「打开自己的 USB PHY / 发出 HSIC connect」门控在「收到 AP 的 HOST_READY」上。** | ★★★☆☆ 字符串级证据（未反汇编 AMSS），是**假设**，但可被 §5.1 的 printk 直接证实/证伪 |

---

## 1. 分析方法与函数地图

### 1.1 恢复函数边界

二进制 stripped，但 **`.eh_frame` 完整**，其 FDE 的 `pc=a..b` 就是真实函数区间（`.text` 被 14 个 FDE 无缝覆盖）：

| 范围 | 我给的命名 | 判定依据（引用到的字符串） |
|---|---|---|
| `0x1450..0x157c` | `alog()` | `/dev/kmsg`, `[ALOG] ` — 内部日志：`__android_log_print` + 写 `/dev/kmsg` |
| `0x157c..0x1c4c` | `load_configureation_via_esoc()` | `ESOC framework not detected`, `load_configureation_via_esoc` |
| `0x1c4c..0x1f20` | `main()` | `Starting MDM helper`, `mdm_helper_proxy` |
| `0x1f20..0x1ff0` | `modem_proxy_routine()` | `/dev/subsys_%s`, `Proxy thread failed to open esoc node` |
| `0x1ff0..0x24a8` | **`modem_state_machine()`** | 6 状态跳转表 |
| `0x24a8..0x2698` | **`peripheral_cmd()`** | `No device specified.`, `Bind/Unbind not supported on this target` |
| `0x2698..0x2e84` | **`mdm9k_powerup()`**（含内联的 post-powerup / EFS 同步段） | `mdm9k_powerup`, `MDM2AP_STATUS is now high` |
| `0x2e84..0x326c` | **`configure_flashless_boot_dev()`** | `configure_flashless_boot_dev`, `Setting up %s boot link` |
| `0x326c..0x3658` | **`load_sahara_images()`** | `Loading Sahara images11111`, `Running '%s'` |
| `0x3658..0x38b4` | **`WaitForCOMport()`** | `Testing if port "%s" exists` |
| `0x38b4..0x39e0` | `mdm9k_shutdown()` | `mdm9k_shutdown` |
| `0x39e0..0x3bc4` | **`mdm9k_monitor()`** | `mdm9k_monitor`, `Monitoring mdm` |
| `0x3bc4..0x3ddc` | `mdm9k_ramdump_collect()` | `mdm9k_ramdump_collect` |
| `0x3ddc..0x3ef0` | `mdm9k_cleanup()/fail()` | `mdm-helper reached fail state` |

（`0x1360..0x1450` 是 `_start` / libc 启动胶水，无 FDE。）

### 1.2 关键静态数据（`.data` 段，VMA 与文件偏移相同）

**ops 函数指针表 @ `0x7008`**（`0x48` = 72 字节，9 项；`0x1900` 处 `memcpy(dst, &0x7008, 0x48)`）：

```
0x7008 0x2698  power_up          -> 复制到 dev+0xc8
0x7010 0x38b4  shutdown          -> dev+0xd0
0x7018 0x39e0  monitor           -> dev+0xd8   （即 mdm9k_monitor）
0x7020 0x0000  reboot            -> dev+0xe0   = NULL
0x7028 0x0000  prep_for_ramdump  -> dev+0xe8   = NULL
0x7030 0x3bc4  ramdump_collect   -> dev+0xf0
0x7038 0x0000  post_ramdump      -> dev+0xf8   = NULL
0x7040 0x3ddc  cleanup/fail      -> dev+0x100
0x7048 0x0000                    -> dev+0x108
```

**private data 表 @ `0x7050`**（`dev->private_data` = `dev+0x110` = `&0x7050`；
赋值点 `0x1a64: adrp x8,#0x6000; ldr x8,[x8,#0xeb0] (=0x7050); str x8,[x19,#0x110]`）：

```
priv+0x00  0x0000000000000001
priv+0x08  0x24a8  == peripheral_cmd() 函数指针
priv+0x10  0x45df  "/dev/ks_hsic_bridge"     ← flashless_boot_device
priv+0x18  0x45f3  "/dev/efs_hsic_bridge"    ← efs_sync_device
priv+0x20  0x4608  "/sys/bus/platform/drivers/s5p-ehci/bind"
priv+0x28  0x4630  "/sys/bus/platform/drivers/s5p-ehci/unbind"
priv+0x30  0x465a  "15510000.usb"            ← BIND 时写入
priv+0x38  0x465a  "15510000.usb"            ← UNBIND 时写入
priv+0x40  0x0000                            ← loader 的 " -g <v>" 开关
priv+0x48  0x0000                            ← ks 的 "-g <prefix>" 参数（本机为 NULL！）
priv+0x50  0x4667  "/dev/block/modem/"       ← ks 的 "-w" 参数
priv+0x58  0x0000                            ← fork() 出来的 efs-sync 子进程 pid
priv+0x60..0x7f  EFS 临时文件三元组（.tmp / .mbn / bin.tmp）
priv+0x80  0x0000                            （为 0 时跳过整段 EFS 临时文件 -s 映射）
priv+0x90..  efs2 / efs3 / acdb 的 .tmp 路径
priv+0x110+ **12 项 Sahara 镜像表**（每项 0x10：{u32 id, pad, char *name}）@ `0x7160`：
  21 sbl1.mbn / 25 tz.mbn / 30 sdi.mbn / 23 rpm.mbn / 31 mba.mbn / 8 qdsp6sw.mbn /
  28 dsp2.mbn / 6 apps.mbn / 16 /dev/block/modem/m9kefs1 / 17 .../m9kefs2 /
  20 .../m9kefs3 / 29 acdb.mbn
```

**PLT 映射**（`.plt` 起始 `0x10e0`，顺序即 `.rela.plt` 顺序）：
`0x1140 open`、`0x1150 __write_chk`、`0x1160 __strlen_chk`、`0x1170 close`、
`0x11a0 __android_log_print`、`0x1200 strncmp`、`0x1270 __errno`、`0x1280 strerror`、
`0x1290 __snprintf_chk`、`0x12a0 ioctl`、`0x12b0 property_get`、`0x12c0 usleep`、
`0x12d0 property_set`、`0x12e0 fork`、`0x12f0 execve`、`0x1300 _exit`、
`0x1310 __strlcat_chk`、`0x1320 system`、`0x1330 stat`、`0x1340 fopen`、`0x1350 fclose`。

### 1.3 ioctl 编号（与 `include/uapi/linux/esoc_ctrl.h` 逐一对齐）

`_IOC(dir,type=0xCC,nr,size=4)`：`_IOW=0x40000000|(4<<16)|(0xCC<<8)|nr`，`_IOR=0x80000000|...`

| 反汇编里的立即数 | 宏 | 出现位置 |
|---|---|---|
| `0x4004CC01` | `ESOC_CMD_EXE` | （本 binary 未用；`esoc_reset.c` 用） |
| `0x8004CC02` | `ESOC_WAIT_FOR_REQ` | `0x2868`, `0x2b04`(+2 → GET_STATUS 的基址), `0x3078`, `0x3a40` |
| `0x4004CC03` | `ESOC_NOTIFY` | `0x2d44`, `0x2e10`, `0x3064`, `0x2ad8`, `0x2c20` |
| `0x8004CC04` | `ESOC_GET_STATUS` | `0x2b04`（`add w1, w27, #2`） |
| `0x8004CC06` | `ESOC_WAIT_FOR_CRASH` | （本 binary 未用；`esoc_reqeng.c` 用） |
| `0x0000CC07` | `ESOC_REG_REQ_ENG` | `0x2858` |
| `0x0000CC0C` | `ESOC_SET_HSIC_READY` | `0x3208` |

`ESOC_NOTIFY` 的取值（`enum esoc_notify`）：

| val | 宏 | 本 binary 里出现的位置 |
|---|---|---|
| 1 | `ESOC_IMG_XFER_DONE` | `0x2ac8`（Sahara 完成之后） |
| 2 | `ESOC_BOOT_DONE` | `0x2d38`（fork 出 ks 之后） |
| 3 | `ESOC_BOOT_FAIL` | `0x2c18`（STATUS 61 秒没起来） |
| **4** | **`ESOC_IMG_XFER_RETRY`** | **`0x2eb4/0x3070`（MODE_BOOT 循环里，软复位 modem）** |
| 5 | `ESOC_IMG_XFER_FAIL` | `0x29cc` / `0x2c1c` |
| 7 | `ESOC_DEBUG_DONE` | `0x3cfc`（ramdump 收完） |
| 12 | `ESOC_DIAG_DISABLE` | `0x2e04` |

---

## 2. ★ 问题 A1：`peripheral_reset()` 到底做什么

### 2.1 答案

**`peripheral_reset(dev)` ≡ `peripheral_cmd(dev, PERIPHERAL_CMD_UNBIND)` → `usleep(10000)` → `peripheral_cmd(dev, PERIPHERAL_CMD_BIND)`。**

**它不等价于「额外做了什么」——除了 10 ms 的延时，它与手工 unbind/bind 完全等价。**
没有 PHY 寄存器、没有 PMU 寄存器、没有 `/sys/.../usb2phy`、没有 `15530000.usb2phy`、
没有 `mdm_hsic_pm` 的 `mdmpm_pdata` 节点、没有 `/sys/kernel/debug/gpio`。

### 2.2 反汇编证据

**`peripheral_cmd(dev, cmd)` @ `0x24a8`（函数体到 `0x2698`，FDE 边界）**

```asm
0x24a8  stp x22,x21,[sp,#-0x30]!
0x24b8  cbz x0, #0x250c              ; if (!dev) -> "No device specified."  return 1
0x24bc  ldr x20,[x0,#0x110]          ; priv = dev->private_data
0x24c0  cbz x20,#0x254c              ; if (!priv) -> "No private data."    return 1
0x24c4  ldr x0,[x20,#0x20]           ; priv+0x20 = bind_node
0x24c8  cbz x0, #0x2520
0x24cc  ldr x8,[x20,#0x28]           ; priv+0x28 = unbind_node
0x24d0  cbz x8, #0x2520
0x24d4  ldr x9,[x20,#0x30]           ; priv+0x30 = bind_cmd
0x24d8  cbz x9, #0x2520
0x24dc  ldr x9,[x20,#0x38]           ; priv+0x38 = unbind_cmd
0x24e0  cbz x9, #0x2520              ; 任一为 NULL -> "Bind/Unbind not supported on this target"(lv6) return 1
0x24e4  cmp w1,#2 ; b.eq #0x2584     ; ---- cmd == 2 : PERIPHERAL_CMD_UNBIND ----
0x24f0  cmp w1,#1 ; b.ne #0x25cc     ; cmd not in {1,2} -> "Unrecognised command." return -1
; ---- cmd == 1 : PERIPHERAL_CMD_BIND ----
0x24f4  mov w1,#1                    ; O_WRONLY
0x24f8  bl  #0x1140                  ; open(priv[4]=bind_node, O_WRONLY)
0x24fc  mov w19,w0
0x2504  ldr x20,[x20,#0x30]          ; buf = priv[6] = "15510000.usb"
0x2508  b   #0x259c
; ---- unbind path ----
0x2584  mov w1,#1
0x2588  mov x0,x8                    ; priv[5] = unbind_node
0x258c  bl  #0x1140                  ; open(unbind_node, O_WRONLY)
0x2594  tbnz w19,#0x1f,#0x25f8       ; open 失败 -> "Failed to open bind node : %s"
0x2598  ldr x20,[x20,#0x38]          ; buf = priv[7] = "15510000.usb"
; ---- 公共写路径 ----
0x259c  mov x1,#-1
0x25a0  mov x0,x20
0x25a4  bl  #0x1160                  ; strlen("15510000.usb") == 12
0x25a8  mov x2,x0                    ; count = 12
0x25ac  mov x3,#-1
0x25b0  mov w0,w19                   ; fd
0x25b4  mov x1,x20                   ; buf
0x25b8  bl  #0x1150                  ; __write_chk(fd, "15510000.usb", 12, -1)
0x25bc  tbnz x0,#0x3f,#0x2644        ; 写失败 -> "Failed to write to bind node: %s"
0x25c0  mov w0,w19
0x25c4  bl  #0x1170                  ; close(fd)
0x25c8  b   #0x2544                  ; return 0
```

**关键字符串（`.rodata` 原始偏移，可直接在二进制里搜索）**

```
0x4608  "/sys/bus/platform/drivers/s5p-ehci/bind"
0x4630  "/sys/bus/platform/drivers/s5p-ehci/unbind"
0x465a  "15510000.usb"
0x531a  "No device specified."
0x532f  "No private data."
0x5340  "Bind/Unbind not supported on this target"
0x5369  "Failed to open bind node : %s"
0x5387  "Failed to write to bind node: %s"
0x53a8  "Unrecognised command."
```

**`peripheral_reset()` 被内联在 `configure_flashless_boot_dev()` 的 `MODE_RUNTIME` 分支 `0x31a8`：**

```asm
0x316c  cmp w20,#2 ; b.ne #0x321c    ; if (mode != MODE_RUNTIME) return 0
0x3174  ...  log(4,"%s: Setting up %s link for efs_sync", dev, dev->link)
0x31a8  ldr x20,[x19,#0x110]         ; priv
0x31ac  ldr x8,[x20,#8]              ; priv[1] == &peripheral_cmd  (0x24a8)
0x31b0  cbz x8,#0x31d8
0x31b4  mov w1,#2
0x31b8  mov x0,x19
0x31bc  blr x8                       ; peripheral_cmd(dev, UNBIND=2)
0x31c0  mov w0,#0x2710               ; 10000
0x31c4  bl  #0x12c0                  ; usleep(10000)   <-- 10 ms
0x31c8  ldr x8,[x20,#8]
0x31cc  mov w1,#1
0x31d0  mov x0,x19
0x31d4  blr x8                       ; peripheral_cmd(dev, BIND=1)
0x31d8  ...  log(4,"%s: Sending boot status notification to HSIC", dev)
0x3204  ldr w0,[x19,#0x80]           ; dev->device_descriptor
0x3208  mov w1,#0xcc0c
0x320c  bl  #0x12a0                  ; ioctl(fd, ESOC_SET_HSIC_READY)
0x3210  tbnz w0,#0x1f,#0x324c        ; <0 -> "%s: %s:hsic_ready failed" return 1
0x3214  mov w0,wzr
0x3218  b   #0x2f8c                  ; return 0
```

### 2.3 三方独立互证

**(a) 原厂 C 源码**（[武林网镜像全文](https://www.vevb.com/wen/2019/11-10/149752.html)，与 CSDN 那篇是同一篇但**这一份没有被截断**）：

```c
pdata->peripheral_cmd(dev,PERIPHERAL_CMD_UNBIND);
//这个会跑到 mdm_hsic_peripheral_cmd() 里边执行如下操作：
case PERIPHERAL_CMD_UNBIND:
    //transport_unbind_node   = "/sys/bus/platform/drivers/s5p-ehci/unbind"
    //transport_unbind_command = "15510000.usb"
    fd = open(pdata->transport_unbind_node, O_WRONLY);
    if (fd < 0) { ALOGE("Failed to open bind node : %s", strerror(errno)); goto error; }
    if(write(fd, pdata->transport_unbind_command,
             strlen(pdata->transport_unbind_command)) < 0) {
        ALOGE("Failed to write to bind node: %s", strerror(errno)); goto error; }
    break;
```

**(b) 原厂真机内核 log**（同一篇文章内，Exynos5433 + MDM9x25 的 trelte）：

```
[12][10.208157][4:mdm_helper:3137][ALOG] MDM9x35: configure_flashless_boot_dev: Initiating HSIC unbind
[ 6][10.208539][4:mdm_helper:3137] s5p-ehci 15510000.usb: remove, state 1
[ 6][10.208605][4:mdm_helper:3137] usb usb1: USB disconnect, device number 1
[ 6][10.306512][3:mdm_helper:3137] s5p-ehci 15510000.usb: stop
[ 6][10.307902][3:mdm_helper:3137] s5p-ehci 15510000.usb: USB bus 1 deregistered
[ 7][10.307981][3:mdm_helper:3137] samsung-usb2phy 15530000.usb2phy: samsung_usb2phy_shutdown: End of setting for shutdown
[ 6][10.308455][3:mdm_helper:3137] ext-mdm qcom,mdm1.50: mdm_toggle_soft_reset
...
[12][10.829278][3:mdm_helper:3137][ALOG] MDM9x35: configure_flashless_boot_dev: Initiating HSIC bind
[ 3][10.829789][3:mdm_helper:3137] s5p-ehci 15510000.usb: s5p_ehci_probe
[ 7][10.831096][3:mdm_helper:3137] samsung-usb2phy 15530000.usb2phy: Can't configure specified phy mode
[ 7][10.831145][3:mdm_helper:3137] samsung-usb2phy 15530000.usb2phy: end of samsung_usb2phy_init
[ 6][10.831174][3:mdm_helper:3137] s5p-ehci 15510000.usb: EHCI Host Controller
[ 6][10.831255][3:mdm_helper:3137] s5p-ehci 15510000.usb: new USB bus registered, assigned bus number 1
...
[12][10.851554][3:mdm_helper:3137][ALOG] Testing if port "/dev/ks_hsic_bridge" exists
[12][10.851813][3:mdm_helper:3137][ALOG] Couldn't find "/dev/ks_hsic_bridge", 1 of 5...
[ 6][11.128323][1:khubd:686] usb 1-2: New USB device found, idVendor=05c6, idProduct=9008
[ 7][11.130791][1:khubd:686] ks_bridge 1-2:1.0: usb_probe_interface
```

→ **`samsung-usb2phy` 的 shutdown/init 全部是 `s5p-ehci` driver 的 unbind/bind 副作用**，
`mdm_helper` 自己**没有**碰 PHY。日志里 unbind 到 bind 之间隔了 621 ms
（`10.208 → 10.829`），其中包含 `ehci_stop` + `usb2phy_shutdown` 以及
`usleep(500000)`——**这 500 ms 是 `configure_flashless_boot_dev` 里写的 `usleep(500000)`**。

### 2.4 「它有没有等 GPIO / 等某个条件」

**没有。** 我把整个 binary 的字符串表逐条列出来了（`strings -t x` 全量，见 §6 命令），
`mdm_helper` 触碰的**全部**文件路径只有：

```
/dev/kmsg
/dev/esoc-0                     (dynstr "/dev/%s"; 另有 /dev/subsys_%s)
/dev/ks_hsic_bridge
/dev/efs_hsic_bridge
/dev/diag
/sys/bus/platform/drivers/s5p-ehci/bind
/sys/bus/platform/drivers/s5p-ehci/unbind
/dev/block/modem/m9kefs1|2|3
/data/misc/mdmhelperdata/m9k0efs{1,2,3}.tmp, m9k0efs{1,2,3}bin.tmp, m9k0acdb.tmp
/firmware/image/                （由 libmdmdetect 填到 dev+0xa8）
/system/bin/ks
```

**没有** `gpio`、`usb2phy`、`15530000`、`hsicctl`、`devicerdy`、`hostwake`、`mdmpm`、`pmu`、
`regulator`、`clk` 任何一个字符串。

「等条件」只有三种机制，全部通过 ioctl / stat：
1. `ioctl(fd, ESOC_WAIT_FOR_REQ)` —— **阻塞**在内核 waitqueue 上（`esoc_dev.c` 用
   `wait_event_interruptible(esoc_udev->req_wait, !kfifo_is_empty(&req_fifo))`）；
2. `ioctl(fd, ESOC_GET_STATUS)` —— 61 次 × 1 s 轮询，等 `status == 1`；
3. `WaitForCOMport()` —— `stat()` / `fopen()` 轮询，间隔 0.5 s。

GPIO 的读取与等待全在**内核**：`mdm_pblrdy_change()`（PBLRDY IRQ → 投 `ESOC_REQ_IMG`）、
`mdm_status_change()`（MDM2AP_STATUS IRQ → `mdm->ready` / `ESOC_UNEXPECTED_RESET`）、
`mdm_set_hsic_ready()`。`mdm_helper` 只是被动消费。

---

## 3. ★ 问题 A2：`MODE_RUNTIME` 之后到结束的完整调用序列

### 3.1 先补全 `mdm9k_powerup()` 全貌（`0x2698..0x2ca4`）

`configure_flashless_boot_dev()` 有两个调用点，模式常量已由原厂 C 源码确认：

* `MODE_BOOT = 1`（C 源码里叫 `MODE_BOOT`，反汇编判据 `orr w8,w20,#2; cmp w8,#3` ⇒ mode∈{1,3}）
* `MODE_RUNTIME = 2`
* `MODE_RAMDUMP = 3`（判据 `cmp w20,#3; b.eq skip` 跳过 `req==ESOC_REQ_IMG` 检查，
  与 C 源码 `if (req != ESOC_REQ_IMG && (mode != MODE_RAMDUMP))` 逐字对应）

```
0x2774  priv = dev->private_data;           // 为 NULL -> "Private data not found"
0x277c  构造 ks argv（见 §3.3）
0x2814  if (g_counter[.bss 0x7240] == 0) {  // 只有第一次上电才做
0x2844      fd = open(dev+0x20 /* "/dev/subsys_mdm9x35" */, O_NONBLOCK=0x800)
0x2850      dev->device_descriptor = fd;    // dev+0x80
0x2858      ioctl(fd, ESOC_REG_REQ_ENG)     // 0xCC07
0x2864      ioctl(fd, ESOC_WAIT_FOR_REQ, &req)
0x2880      if (req != 1) -> "Expecting ESOC_REQ_IMG. Recieved : %u"
        }
0x2888  configure_flashless_boot_dev(dev, MODE_BOOT=1)      // 见 §3.2
0x2894  if (ret) -> "Link setup failed"  return 1
0x2898  if (persist.mdm_boot_debug == "true") { 打印 "mdm boot paused" ... 
        每 15000*? 循环等 /data 属性变成 "resume" 后才 property_set("persist.mdm_boot_debug","true") }
0x2950  if (property "resume_rma") { "rma boot...reinitializing boot link";
                                      configure_flashless_boot_dev(dev, MODE_BOOT=1) 再一次 }
0x2a08  load_sahara_images(dev, g_counter ? "-i" : "")       // 见 §3.4
0x2ac8  val=1 ; ioctl(fd, ESOC_NOTIFY, &val)   // ★ ESOC_IMG_XFER_DONE
0x2ae8  if (<0) -> "Failed to send IMG_XFER_DONE notification"
0x2aec  for (i=0; i<0x3d; i++) {                       // 最多 61 次
0x2b00      ioctl(fd, ESOC_GET_STATUS, &status)        // 0x8004CC04
0x2b10      if (status == 1) goto 0x2bc0               // MDM2AP_STATUS 已高
0x2b20      log(4,"%s: Waiting for mdm boot", dev)
0x2b40      usleep(0x000F4240 = 1000000)               // 1 s
        }
0x2bf4  log(6,"MDM did not set MDM2AP_STATUS high"); val=3
0x2c1c  ioctl(fd, ESOC_NOTIFY, &val)                   // ESOC_BOOT_FAIL
0x2bc0  log(4,"%s: MDM2AP_STATUS is now high", dev)
0x2c4c  if (strncmp(dev+0x40 /*dev->link*/, "HSIC", 5) == 0) {
0x2c64      configure_flashless_boot_dev(dev, MODE_RUNTIME=2)   // §3.2
0x2c70      if (ret) -> "Link setup failed"
        }
0x2cd0  /* ---------- post-powerup / EFS 同步段 ---------- */
0x2cd0  x0 = priv+0x18  /* "/dev/efs_hsic_bridge" */
0x2cd4  if (!x0) -> "%s: No efs_sync_device specified for target" (返回 1)
0x2ce0  WaitForCOMport("/dev/efs_hsic_bridge", 75, 0)   // ★ 75 × 0.5 s = 37.5 s
0x2ce4  if (ret) -> "%s: Could not detect EFS sync port"  (返回 1)   ★ 不会发 BOOT_DONE
0x2d10  pid = fork();  priv+0x58 = pid                  // ★ 先 fork，后发 BOOT_DONE
0x2d18  if (pid < 0) -> "%s: Failed to create efs sync process"
0x2d1c  if (pid == 0) {                                 // ---- 子进程 ----
0x2d20      execve("/system/bin/ks", argv /*§3.3*/, NULL)
0x2d34      if (<0) -> "%s: Failed to exec KS process for efs sync"; _exit(127)
        }
0x2d38  val = 2 ; ioctl(fd, ESOC_NOTIFY, &val)          // ★ ESOC_BOOT_DONE
0x2d54  if (<0) -> "Failed to send ESOC_BOOT_DONE notification"
0x2d58  fdiag = open("/dev/diag", O_NONBLOCK)
0x2d6c  if (fdiag < 0) {
0x2dd0      log(6,"%s: Diag node is not exist. fd %d", dev, fdiag)
0x2e04      val = 12 ; ioctl(fd, ESOC_NOTIFY, &val)     // ESOC_DIAG_DISABLE
0x2e38      if (<0) -> "Failed to send ESOC_DIAG_DISABLE notification"
        } else { close(fdiag); log(4,"%s: Diag node is exist.") }
0x2e24  g_counter[0x7240]++ ; return 0
```

### 3.2 `configure_flashless_boot_dev(dev, mode)` 全貌（`0x2e84..0x326c`）

```c
if (!dev)                          -> "Device structure passed in as NULL",      return 1
priv = dev->private_data;
if (!priv)                         -> "%s: %s: Private data is null",           return 1
if (strncmp(dev->link, "HSIC", 5)) -> "%s: Link %s not supported by mdm-helper", return 1

if (mode == MODE_BOOT(1) || mode == MODE_RAMDUMP(3)) {
    log(4, "%s: Setting up %s boot link", dev->name, dev->link);
    for (i = 0; i < 50 /* NUM_LINK_RETRIES */; ++i) {          // 0x314c: cmp w24,#0x32
        if (priv->peripheral_cmd) {
            log(4, "%s: %s: Initiating HSIC unbind", dev->name, "configure_flashless_boot_dev");
            priv->peripheral_cmd(dev, 2);                       // UNBIND  -> s5p-ehci/unbind
        }
        int cmd = 4;                                            // ESOC_IMG_XFER_RETRY
        if (ioctl(fd, ESOC_NOTIFY, &cmd) < 0)                   // ★ 内核: mdm->init=1; mdm_toggle_soft_reset()
            -> "%s: :%s: Failed to reset mdm",                  return 1
        int req = 0;
        if (ioctl(fd, ESOC_WAIT_FOR_REQ, &req) < 0)             // ★ 阻塞等 PBLRDY 中断投 ESOC_REQ_IMG
            -> "%s: %s:wait for image xfer fail",               return 1
        if (req != 1 /*ESOC_REQ_IMG*/ && mode != 3) {
            log(6, "%s: %s: Unnexpected request: %d");  continue;   // 重试
        }
        usleep(500000);                                         // ★ 0.5 s
        if (priv->peripheral_cmd) {
            log(4, "%s: %s: Initiating HSIC bind", ...);
            priv->peripheral_cmd(dev, 1);                       // BIND    -> s5p-ehci/bind
        }
        if (WaitForCOMport(priv->flashless_boot_device /* /dev/ks_hsic_bridge */, 5, 0) == 0)
            break;                                              // 成功
    }
    if (rcode != 0) -> "%s: %s: Failed to setup HSIC link",     return 1
}
else if (mode == MODE_RUNTIME(2)) {
    log(4, "%s: Setting up %s link for efs_sync", dev->name, dev->link);
    peripheral_reset(dev);                                      // UNBIND -> usleep(10000) -> BIND
    log(4, "%s: Sending boot status notification to HSIC", dev->name);
    if (ioctl(fd, ESOC_SET_HSIC_READY /*0xCC0C*/) < 0)          // ★ 内核: gpio 0 -> msleep(10) -> gpio 1
        -> "%s: %s:hsic_ready failed",                          return 1
}
else return 0;
return 0;
```

### 3.3 `fork()+execve()` 的 argv（本机实测值）

argv 在 `0x277c..0x2810` 构造于 `sp+0x30`，在 `0x2d2c` 作为 `execve` 的 `argv` 传入：

```c
argv[0] = "/system/bin/ks";
argv[1] = "-m";
argv[2] = "-p";
argv[3] = priv+0x18            = "/dev/efs_hsic_bridge";
argv[4] = "-w";
argv[5] = priv+0x50            = "/dev/block/modem/";          // ★ 不是 /cpdump/
argv[6] = "-t";
argv[7] = "-1";
argv[8] = "-l";
argv[9] = (priv+0x48 != 0) ? "-g" : NULL;                      // ★ 本机 priv+0x48 == 0 ⇒ NULL
argv[10]= priv+0x48;                                           //    ⇒ 不传 -g，argv 到 9 就结束
argv[11]= NULL;
envp    = NULL;
```

**实际执行的命令（G9209 这一份 private data 的精确值）：**

```
/system/bin/ks -m -p /dev/efs_hsic_bridge -w /dev/block/modem/ -t -1 -l
```

> 差别提醒：`STATUS-当前进展.md` §三 写的
> `ks -m -p /dev/efs_hsic_bridge -w /cpdump/ -t -1 -l -g m9k0`
> 是**另一台机型的 private data**。本机 `priv+0x48 = 0x7098 = NULL`、`priv+0x50 = 0x70a0 = "/dev/block/modem/"`。
> 所以本机**没有 `-g`**，`-w` 的值是 `/dev/block/modem/`。

### 3.4 `load_sahara_images(dev, arg2)`（`0x326c..0x3658`）—— 顺带发现

```
0x32d0  WaitForCOMport(priv+0x10 "/dev/ks_hsic_bridge", 10, 1)   // ★ open_for_read=1 → fopen("r")
0x32e0  if (ret==1) -> "%s: Could not find flashless boot port"
0x3300  snprintf(cmd, 0x800, "%s %s -w %s -p %s -r %d",
                 "/system/bin/ks", arg2, dev+0x88, priv+0x10, 21)
0x3340  if (priv+0x40) strlcat(cmd, " -g ", ...), strlcat(cmd, priv+0x40, ...)
0x3374  for each of the 12 entries {id, name} in priv+0x110:
            snprintf(t, 0x100, " -s %d:%s%s", id,
                     name[0]=='/' ? "" : dev+0xa8 /*"/firmware/image/"*/, name)
            strlcat(cmd, t, 0x800)
0x33e4  if (priv+0x80) { 追加 3 条 efs 的 " -s %d:%s"（.tmp 路径） }   // 本机 priv+0x80==0 → 跳过
0x34ac  if (priv+0x100) { 追加 acdb 的 " -s %d:%s" }                   // 本机 priv+0x100==0 → 跳过
0x355c  log(4, "%s: Running '%s'", dev, cmd)
0x3584  w20 = system(cmd)                    // 用 system()，不是 fork/execve
0x3594  log(4, "%s: Running Done'%s'", dev, cmd)
0x35c0  property_set("debug.mdm.cpboot_done", "true")
0x35dc  if (w20 == 0)      -> log(6,"%s: Sahara transfer completed successfully")  return 0
0x35e0  if (w20 == 0x500)  -> "%s: ERROR: RAM dumps were forced unexpectedly"     return 1
0x3620  else               -> "%s: ERROR: ks return code was %d"                  return 1
```

**展开后的原厂命令行（本机，12 条 `-s` 的顺序就是镜像表顺序）：**

```
/system/bin/ks -i -w /cpdump/ -p /dev/ks_hsic_bridge -r 21 \
  -s 21:/firmware/image/sbl1.mbn  -s 25:/firmware/image/tz.mbn \
  -s 30:/firmware/image/sdi.mbn   -s 23:/firmware/image/rpm.mbn \
  -s 31:/firmware/image/mba.mbn   -s 8:/firmware/image/qdsp6sw.mbn \
  -s 28:/firmware/image/dsp2.mbn  -s 6:/firmware/image/apps.mbn \
  -s 16:/dev/block/modem/m9kefs1  -s 17:/dev/block/modem/m9kefs2 \
  -s 20:/dev/block/modem/m9kefs3  -s 29:/firmware/image/acdb.mbn
```
（`-w` 的值 = `dev+0x88`，由 `libmdmdetect.so` 的 `get_system_info()` 填；该 .so 里的
ram-dump-path 字符串是 `/cpdump/`，故此处标为 `/cpdump/`；`-i` 来自调用点 `0x2a10`，
第一次上电（`g_counter==0`）时该参数是 `0x4a49` = **空串**。）

> `-r 21` = `--ramdumpimage`，"Image ID which must be transferred before forcing Sahara memory
> dump mode"，取值 21 正好等于 sbl1 的 id。

### 3.5 ★ 精确到「动作 / 目标 / 参数 / 延时」的原厂时序表（可直接照抄实现）

时钟基准：以 `open("/dev/subsys_mdm9x35")` 触发的 `ESOC_PWR_ON` 为 T0。
`t` 为原厂真机 log 的实测相对时间（来自 §2.3(b)）。

| # | t | 动作 | 目标 / 参数 | 之后延时 | 备注 |
|---|---|---|---|---|---|
| 0 | — | `mdm_helper_proxy` 线程 `open("/dev/subsys_mdm9x35", O_RDONLY)` 并**永久常驻持 fd** | 原厂源码 `modem_proxy_routine()`：`do { sleep(50000); } while(1);` | — | 我们的 `esoc_pwron_hold` 对应此步 ✅ |
| 1 | — | `open("/dev/esoc-0", O_NONBLOCK)` | `dev->device_descriptor = dev+0x80` | — | |
| 2 | 9.766 | `ioctl(fd, ESOC_REG_REQ_ENG /*0xCC07*/)` | — | 阻塞 | |
| 3 | 10.207 | `ioctl(fd, ESOC_WAIT_FOR_REQ /*0x8004CC02*/, &req)` | 期待 `req == 1 (ESOC_REQ_IMG)` | — | **阻塞**，由 PBLRDY IRQ 唤醒 |
| 4 | 10.208 | `open("/sys/bus/platform/drivers/s5p-ehci/unbind", O_WRONLY)` | + `write(fd, "15510000.usb", 12)` + `close` | — | `MODE_BOOT` 循环开始（最多 **50** 轮） |
| 5 | 10.308 | `ioctl(fd, ESOC_NOTIFY /*0x4004CC03*/, &4)` | `4 = ESOC_IMG_XFER_RETRY` | 阻塞 | ★ 内核 `mdm->init = 1; mdm_toggle_soft_reset()`：AP2MDM_SOFT_RESET 拉低 **8–9 ms** 再放开 |
| 6 | 10.318 | `ioctl(fd, ESOC_WAIT_FOR_REQ, &req)` | 期待 `req == 1` | — | PBL 起来拉高 PBLRDY（实测 **+20 ms**）→ IRQ 投 `ESOC_REQ_IMG` |
| 7 | 10.329 | `usleep(500000)` | **500 ms** | 500 ms | 等 PBL 稳定 |
| 8 | 10.829 | `open("/sys/bus/platform/drivers/s5p-ehci/bind", O_WRONLY)` | + `write(fd, "15510000.usb", 12)` + `close` | — | 触发 `s5p_ehci_probe` → `samsung_usb2phy_init` → 新 USB bus |
| 9 | 10.851 | `WaitForCOMport("/dev/ks_hsic_bridge", 5, 0)` | `stat()` 轮询，间隔 **0.5 s**，最多 **5** 次 = 2.5 s | 0.5 s/次 | 失败则回到 #4 重试（共 50 轮） |
| 10 | 11.13 | PBL 以 `05c6:9008` if0 枚举 → `/dev/ks_hsic_bridge` 出现 | — | — | |
| 11 | — | `WaitForCOMport("/dev/ks_hsic_bridge", 10, 1)` | **`fopen("r")` 成功**才算过；最多 5 s | 0.5 s/次 | |
| 12 | — | `system("/system/bin/ks -i -w /cpdump/ -p /dev/ks_hsic_bridge -r 21 -s <12 条映射>")` | 见 §3.4 | — | 走完 12 镜像 + `HELLO mode=1` |
| 13 | — | `property_set("debug.mdm.cpboot_done", "true")` | — | — | |
| 14 | — | `ioctl(fd, ESOC_NOTIFY, &1)` | `1 = ESOC_IMG_XFER_DONE` | — | ★ 必须在 Sahara **之后**（内核据此武装 `mdm2ap_status_check_work`，120 s） |
| 15 | — | `for (i=0;i<61;i++) ioctl(fd, ESOC_GET_STATUS /*0x8004CC04*/, &st)` | `st == 1` 即退出 | **1 s**/次 | 最多 61 s；超时 → `ESOC_NOTIFY(&3/*BOOT_FAIL*/)` |
| 16 | — | `ioctl(fd, ESOC_NOTIFY, &4)`? — **否**，此步不存在 | — | — | 只有 #5 会发 RETRY |
| 17 | — | `open("/sys/bus/platform/drivers/s5p-ehci/unbind", O_WRONLY)` + `write "15510000.usb"` | `MODE_RUNTIME` 开始 | — | 注意顺序：**先 unbind** |
| 18 | — | `usleep(10000)` | **10 ms** | 10 ms | `peripheral_reset` 的固定延时 |
| 19 | — | `open(".../s5p-ehci/bind")` + `write "15510000.usb"` | — | **0 ms（无延时）** | ★ 紧接着就发 #20，中间没有 sleep |
| 20 | — | `ioctl(fd, ESOC_SET_HSIC_READY /*0xCC0C*/)` | — | — | ★ 内核 `mdm_set_hsic_ready()`：`gpio=0; msleep(10); gpio=1` → **上升沿** |
| 21 | — | `WaitForCOMport("/dev/efs_hsic_bridge", 75, 0)` | `stat()` 轮询，间隔 0.5 s，**最多 37.5 s** | 0.5 s/次 | ★ **失败即 return 1，不会发 BOOT_DONE** |
| 22 | — | `fork()`；子进程 `execve("/system/bin/ks", argv, NULL)` | 见 §3.3 | — | ★ **先 fork，再发 BOOT_DONE** |
| 23 | — | `ioctl(fd, ESOC_NOTIFY, &2)` | `2 = ESOC_BOOT_DONE` | — | ★ 内核 `esoc_clink_evt_notify(ESOC_RUN_STATE)` → `complete(&boot_done)` |
| 24 | — | `fd_diag = open("/dev/diag", O_NONBLOCK)` | 成功 → `close` + log；失败 → `ioctl(fd, ESOC_NOTIFY, &12)` | — | `12 = ESOC_DIAG_DISABLE`（**只在打开失败时才发**） |
| 25 | — | `g_counter++`；`mdm9k_powerup()` 返回 0 | — | — | 状态机切到 `POST_POWERUP` |

**★ 关键回答**：`ESOC_SET_HSIC_READY`(#20) 之后**没有任何中间动作**，
`configure_flashless_boot_dev` 立刻 `return 0`，控制权回到 `mdm9k_powerup()` 的 `0x2cd0`，
紧接着就是 `WaitForCOMport("/dev/efs_hsic_bridge", 75, 0)`。
（注意 `ESOC_IMG_XFER_DONE` 是在**早得多**的 #14 发的，与 `SET_HSIC_READY` 之间隔着
`GET_STATUS` 轮询和 `MODE_RUNTIME` 的 unbind/bind。）

---

## 4. ★ 问题 A3：`mdm9k_monitor()` 与主状态机

### 4.1 状态表（跳转表 @ `0x3ef0`，6 项 32-bit 相对偏移）

```
0x3ef0: c8e1ffff 3ce2ffff 80e2ffff c8e2ffff 0ce3ffff f8e4ffff
state 0 -> 0x20b8   POWERUP
state 1 -> 0x212c   POST_POWERUP / MONITOR
state 2 -> 0x2170   RAMDUMP
state 3 -> 0x21b8   REBOOT
state 4 -> 0x21fc   SHUTDOWN
state 5 -> 0x23e8   FAILED
```

`0x1ff0: log(4,"Starting %s", dev)`，`w23 = 0`（初始 state = POWERUP），
`0x20a0: ldrsw x8,[0x3ef0 + w23*4]; add x8,x8,#0x3ef0; br x8`。

### 4.2 各状态的动作（`dev+0x84` = `required_action`，`dev+0xd8` = `mdm9k_monitor`）

```
state 0 POWERUP (0x20b8)
    log "switching state to POWERUP"
    r = ops->power_up(dev)                 // mdm9k_powerup()
    if (r) -> log(6,"Powerup failed") -> state = 5 FAILED
    else   -> state = (dev->required_action == 2) ? 2 : 1

state 1 POST_POWERUP/MONITOR (0x212c)
    log "switching state to POST POWERUP/monitor"
    r = ops->monitor(dev)                  // ★ mdm9k_monitor() —— 阻塞
    if (r) -> log(6,"Post power_up failed") -> state = 5 FAILED
    switch (dev->required_action) {
      case 1: dev->required_action = 0; state = 0; break;   // 重新下载
      case 2: dev->required_action = 0; state = 2; break;   // ramdump
      case 3: if (!ops->shutdown) -> log "No shutdown function present" -> FAILED
              dev->required_action = 0; state = 4; break;
      default: log(6,"post pwrup returned unsupported action:%d") -> FAILED }

state 2 RAMDUMP (0x2170)
    log "Switching state to RAMDUMP"
    if (ops->prep_for_ramdump) r = prep_for_ramdump(dev)      // 本机为 NULL，跳过
    if (ops->ramdump_collect) r = ramdump_collect(dev)        // mdm9k_ramdump_collect()
    if (ops->post_ramdump)    r = post_ramdump(dev)           // 本机为 NULL，跳过
    state = (dev->required_action == 3) ? 4 : 0

state 3 REBOOT (0x21b8)
    log "Normal reboot request"
    if (!ops->reboot) -> log(6,"Reboot function not defined") -> FAILED   // 本机 NULL

state 4 SHUTDOWN (0x21fc)
    log "Handling shutdown request"
    if (!ops->shutdown) -> "No shutdown function defined" -> FAILED
    r = ops->shutdown(dev)                  // mdm9k_shutdown()
    state = (r == 0) ? 0 : 5

state 5 FAILED (0x23e8)
    log(6,"Reached failed state. exiting")
    if (ops->cleanup) { log "Calling cleanup function"; cleanup(dev); }
    else log "No cleanup function defined"
    return
```

### 4.3 ★ `mdm9k_monitor()` 的实现（`0x39e0..0x3bc4`）

```c
int mdm9k_monitor(struct mdm_device *dev)
{
    if (!dev) { log_6("%s: Invalid device structure passed", "mdm9k_monitor"); return 1; }
    log_4("%s: Monitoring mdm", dev->mdm_name);

    uint32_t req = 0;
    if (ioctl(dev->fd, ESOC_WAIT_FOR_REQ /*0x8004CC02*/, &req) < 0) {
        log_6("%s: ESOC_WAIT_FOR_REQ ioctl failed", ...);
        return 1;
    }
    switch (req) {
    case 1: /* ESOC_REQ_IMG      */ log_4("%s: Recieved request to transfer images");
                                     dev->required_action = 1; return 0;
    case 2: /* ESOC_REQ_DEBUG    */ log_4("%s: Recieved request for ramdump collection");
                                     dev->required_action = 2; return 0;
    case 3: /* ESOC_REQ_SHUTDOWN */ log_4("%s: Recieved shutdown request");
                                     dev->required_action = 3; return 0;
    default:                         log_6("%s: Unknown request recieved: %u");
                                     return 0;      /* required_action 不变 */
    }
}
```

### 4.4 ★★ 对「modem 起来之后 AP 该持续做什么」的直接回答

**`mdm_helper` 在 modem ready 之后什么周期性动作都不做。**

* 状态机停在 `state 1 (POST_POWERUP)`；
* `mdm9k_monitor()` 里的 `ESOC_WAIT_FOR_REQ` 在 `esoc_dev.c` 里是
  `wait_event_interruptible(esoc_udev->req_wait, !kfifo_is_empty(&req_fifo))` ——
  **真正的阻塞**，不是轮询，没有超时，没有定时器；
* 因此「每隔多久轮询一次」的答案是：**不轮询**。唯一会唤醒它的是 modem 主动发起、由内核
  `mdm_pblrdy_change()` 投入 req fifo 的请求。

**★ 但这里有一个我们目前完全缺失的联动：**
`mdm_pblrdy_change()` 只在 `mdm->init == 1` 时才投 `ESOC_REQ_IMG`：

```c
static irqreturn_t mdm_pblrdy_change(int irq, void *dev_id) {
    ...
    if (mdm->init) { mdm->init = 0;
        dev_err(dev, "Signaling request engine for images\n");
        esoc_clink_queue_request(ESOC_REQ_IMG, esoc);
        return IRQ_HANDLED; }
    if (mdm->debug) esoc_clink_queue_request(ESOC_REQ_DEBUG, esoc);
    return IRQ_HANDLED;
}
```
`mdm->init` 只有两个来源：
1. `mdm_cmd_exe(ESOC_PWR_ON)`（`esoc-mdm-4x.c:388` 附近）—— 首次上电，我们用了；
2. `mdm_notify(ESOC_IMG_XFER_RETRY)`（`esoc-mdm-4x.c:566`）——**原厂在 `MODE_BOOT` 里
   每轮都主动再武装一次，我们没有。**

→ 结果是：首次 PBLRDY 消耗掉 `mdm->init` 之后，**modem 之后任何一次重新进 PBL
（包括它自己复位后）都不会再向 AP 投 `ESOC_REQ_IMG`**，我们的 `ESOC_WAIT_FOR_REQ`
就再也不会返回。

### 4.5 附：`mdm9k_shutdown()` / `mdm9k_ramdump_collect()`

```c
int mdm9k_shutdown(struct mdm_device *dev) {           // 0x38b4
    if (!dev)  -> "Invalid device structure passed in", return 1
    if (!priv) -> "private data is NULL",               return 1
    if (strncmp(dev->link /*dev+0x40*/, "HSIC", 5) == 0 && priv->peripheral_cmd) {
        log_4("%s: %s: Initiating HSIC unbind", dev->name, "mdm9k_shutdown");
        priv->peripheral_cmd(dev, 2);                  // 只 UNBIND，不 BIND
    }
    mdm9k_monitor(dev);                                // 0x3940 再阻塞一次
    if (dev->required_action == 1) { dev->required_action = 0; return 0; }
    log_6("%s: Invalid request recieved.Expected Image request");
    return 1;                                          // -> FAILED
}

int mdm9k_ramdump_collect(struct mdm_device *dev) {    // 0x3bc4
    if (!dev) -> "%s: Invalid device structure passed", 失败
    dev->required_action = 2;
    if (configure_flashless_boot_dev(dev, 3 /*MODE_RAMDUMP*/))
        -> "%s: Failed to configure hsic port for collecting dumps"
    if (load_sahara_images(dev, "-m"))                 // ★ 注意是 "-m"（memdump），不是 "-i"
        -> "%s: Failed to collect dumps"
    /* 0x3cfc */ ioctl(fd, ESOC_NOTIFY, &7 /*ESOC_DEBUG_DONE*/)
                 -> 失败则 "%s: :%s: Failed to send debug done notification"
    mdm9k_monitor(dev);
}
```

---

## 5. ★ 问题 A4：GPIO / `devicerdy` / `hsicctl`

**`mdm_helper` 完全不涉及。** 见 §2.4 的完整路径清单。

* 它**没有**打开 `/sys/kernel/debug/gpio`；
* 它**没有**任何 `qcom,mdm2ap-devicerdy-gpio` / `qcom,mdm2ap-hostwake-gpio` / `mdmpm_pdata` /
  `hsicctl` / `mdm_hsic_pm` 相关的字符串或路径；
* 唯一与「主机就绪」有关的动作是 `ioctl(fd, ESOC_SET_HSIC_READY)`，
  它把整件事**委托给内核** `mdm_set_hsic_ready()`：

```c
/* drivers/esoc/esoc-mdm-4x.c:783 */
static void mdm_set_hsic_ready(struct esoc_clink *esoc)
{
    struct mdm_ctrl *mdm = get_esoc_clink_data(esoc);
    gpio_set_value(MDM_GPIO(mdm, AP2MDM_HSIC_READY), 0);
    msleep(10);
    gpio_set_value(MDM_GPIO(mdm, AP2MDM_HSIC_READY), 1);
}
```

* 而 `AP2MDM_HSIC_READY` 在此之前被内核强制拉低过两次：
  `mdm_cmd_exe(ESOC_PWR_ON)`（`esoc-mdm-4x.c:388`）与 probe（`:903`）。
  所以只要没人在中间把它拉高，`ESOC_SET_HSIC_READY` 一定产生一个 **0 →(10 ms)→ 1 的上升沿**。

### 5.1 ★ 与「11 秒自复位 + USB 全程为空」的可能因果（假设，需实测确认）

`stock-cp/image/apps.mbn`（本机原厂 CP 包）里含构建路径：

```
/home/dpi/qb5_8814/workspace/ZEROFLTE_CHN_CTC/apps_proc/core/wiredconnectivity/
    hsusb/driver/src/adapt_layers/hsu_al_task.c
```

`ZEROFLTE_CHN_CTC` 正是 SM-G9209（CHN/CTC）。该文件里的符号与日志字符串：

```
SS_Hsic_host_ready_Raising_Signal_Isr
SS_Hsic_host_ready_Falling_Signal_Isr
SS_Verify_Host_Wakeup_Gpio_State
SS_handle_wakeup_gpio_timer
SS_handle_wakeup_gpio
SS_assert_host_wakeup_gpio
SS_drive_Device_Ready_Gpio
SS_GPIO_wakeup_host_cb
SS_GPIO_device_ready_cb
hsu_al_task_main
ss_disable_usb_interrupt / ss_enable_usb_interrupt

" - Rearm Timer : is_host_ready_recvd=%d, is_hwgpio_timer_started=%d"
" - Clear and Stop Timer : is_host_ready_recvd=%d, is_hwgpio_timer_started=%d"
" - HSIC DEVICE_READY Setting HIGH, USB PHY ON : ready=%d"
" - HSIC DEVICE_READY Setting LOW,  USB PHY OFF : ready=%d"
" - make the HSIC connect signal"
" - enter , HSIC HOST WAKEUP Setting HIGH"
```

→ **AMSS 把「打开自己的 USB PHY / 拉起 DEVICE_READY / 发出 HSIC connect」门控在
`is_host_ready_recvd`（即收到 AP 的 `AP2MDM_HSIC_READY` 上升沿）之上**，并且用一个
会 `Rearm Timer` 的定时器跟踪它。这与我们的现象完全吻合：
`usb=[]`（AMSS 的 PHY 从未打开）+ 约 11 s 后 self-reset（那个定时器最终超时）。

**注意这是字符串级证据的假设，我没有反汇编 AMSS 去定位那个超时常量。**
建议按 `STATUS-当前进展.md` §五.1 的计划加 printk 确认 `mdm_set_hsic_ready()`
到底有没有被调用、pin 有没有真的翻转；同时把 SHIC_READY 的时刻往后推（见 §6.2 建议 3）。

---

## 6. ★ 与现状的矛盾（逐条指认）

对照对象：`exp/run-exp8-runtime.sh`、`tools/esoc_reqeng.c`、`tools/mdm_runtime_handshake.c`。

### 6.1 逐条

| # | 严重度 | 原厂做法 | 我们现在做法 | 位置 |
|---|---|---|---|---|
| **C1** | ★★★★★ | `MODE_BOOT` 开始前先 **unbind EHCI**，再 `ESOC_NOTIFY(ESOC_IMG_XFER_RETRY)` 触发**软复位 modem**，再 `WAIT_FOR_REQ`，再 `usleep(500000)`，最后才 **bind EHCI** | 完全缺失。我们是 `esoc_pwron_hold`（内核 `mdm_do_first_power_on`）→ 直接 `ldrx`，EHCI 全程 bound，**`mdm->init` 只被 PWR_ON 武装过一次** | 原厂 `0x301c..0x3138` |
| **C2** | ★★★★★ | 每轮 `MODE_BOOT` 都用 `ESOC_IMG_XFER_RETRY` 重武装 `mdm->init`；因此 modem 每次进 PBL 都会投 `ESOC_REQ_IMG` | 从不重武装 → **modem 自复位后再进 PBL 时内核不会投任何请求**，`ESOC_WAIT_FOR_REQ` 永远不返回 | 内核 `esoc-mdm-4x.c:566` / `:508` |
| **C3** | ★★★★ | `ESOC_IMG_XFER_DONE` 在 **Sahara 全部完成之后** 发（`0x2ac8`），随后才 `GET_STATUS` 轮询等待 STATUS | `esoc_reqeng.c:180` 在**注册 req engine 之后立刻**就发（Sahara 之前、上电之前）。内核 `mdm_notify` 里这一句是 `if (MDM2AP_STATUS == 0) schedule_delayed_work(mdm2ap_status_check_work, 120000)`，早发会白武装一个 120 s 看门狗 | `tools/esoc_reqeng.c:179-181` |
| **C4** | ★★★★ | `WaitForCOMport("/dev/efs_hsic_bridge", **75**, 0)` ⇒ **37.5 s**；且**失败就 `return RET_FAILED`，不发 `ESOC_BOOT_DONE`** | 只等 **15 s**，而且无论成败都继续发 `BOOT_DONE` + `DIAG_DISABLE` | `tools/mdm_runtime_handshake.c:115-133` |
| **C5** | ★★★ | `bind` 写入返回后**立刻**发 `ESOC_SET_HSIC_READY`（零延时） | 中间插了 `sleep_ms(300)` | `tools/mdm_runtime_handshake.c:108-112` |
| **C6** | ★★★ | 从 STATUS 拉高到发 HSIC_READY 之间：`GET_STATUS` 是 **1 秒粒度**轮询（`usleep(1000000)`），外加 `unbind`→`10 ms`→`bind` 的 EHCI 重新 probe（原厂真机实测 unbind→bind 之间 ~620 ms 的驱动动作） | 我们用 500 ms 采样、STATUS 高后 **31 ms** 就 unbind、**379 ms** 就发 HSIC_READY。**比原厂早得多**——如果 AMSS 那时还没注册好 `SS_Hsic_host_ready_Raising_Signal_Isr`，这次上升沿可能被丢掉（见 §5.1） | `tools/mdm_runtime_handshake.c:96-112` |
| **C7** | ★★★ | `ks` 的 EFS 同步参数：`-m -p /dev/efs_hsic_bridge -w /dev/block/modem/ -t -1 -l`，**没有 `-g`** | 文档里写的是 `-w /cpdump/ ... -g m9k0`（那是别的机型的 private data） | `0x27a4..0x2810` + `.data 0x7098/0x70a0` |
| **C8** | ★★★ | `execve` 发生在 `WaitForCOMport(efs,75)` **成功之后**；`ESOC_BOOT_DONE` 发生在 `fork` **之后** | 顺序对（✅），但因为我们从来没能让桥出现，`ks` 从未被拉起 | — |
| **C9** | ★★ | 镜像下载前先 `WaitForCOMport("/dev/ks_hsic_bridge", 10, **1**)` —— **`fopen("r")` 打开成功**才算就绪 | 我们用自己的 `ldrx` 直接 open，没有这一步的等价校验 | `0x32d0` |
| **C10** | ★★ | `ESOC_DIAG_DISABLE`(12) **只在 `open("/dev/diag")` 失败时**才发 | 无条件发 | `tools/mdm_runtime_handshake.c:129-133` |
| **C11** | ★★ | **`/system/bin/ks` 是原厂 efs-sync 程序** | `run-exp8-runtime.sh` 把它覆盖成 `#!/system/bin/sh; exit 0`。即使桥出现了也**不会有任何 EFS 同步发生**（排查阶段可以，但别忘了恢复，且要恢复成 `ks.real`） | `exp/run-exp8-runtime.sh:11-12` |
| **C12** | ★ | `MODE_BOOT` 重试上限 **50** 轮；`GET_STATUS` 轮询上限 **61 次 × 1 s**；`WaitForCOMport` 单次单位 0.5 s | 我们工具里的 15 s / 2400×500 ms 等是自定值 | `0x314c`、`0x2b50`、`0x3724` |

### 6.2 建议的下一步（按性价比排序）

1. **补齐 `MODE_BOOT` 的链路建立**（对应 C1/C2）：在 `esoc_pwron_hold` 上电之后、`ldrx`
   开跑之前插入一组：
   `write /sys/bus/platform/drivers/s5p-ehci/unbind "15510000.usb"`
   → `ioctl(ESOC_NOTIFY, 4)`
   → `ioctl(ESOC_WAIT_FOR_REQ)` 等 `1`
   → `usleep(500000)`
   → `write .../bind "15510000.usb"`
   → `WaitForCOMport("/dev/ks_hsic_bridge", 5, 0)`
   并用 `ESOC_IMG_XFER_RETRY` **替代**我们现在的启动方式（让内核去发软复位），
   这样 `mdm->init` 的武装时序和原厂一致。
2. **把 `ESOC_IMG_XFER_DONE` 移到 Sahara 完成之后**（C3）。
3. **把 HSIC_READY 的时刻推迟到「STATUS 高之后 ≥ 1 s」再试一次**（C6）——
   成本极低，可以直接验证 §5.1 的假设。
4. **`ESOC_SET_HSIC_READY` 后把 efs 桥等待拉到 37.5 s**，并且**桥不出现就不要发
   `BOOT_DONE`**（C4）。
5. **取消 bind 与 HSIC_READY 之间那 300 ms**，与原厂对齐（C5）。
6. 每次实验前确认 `AP2MDM_HSIC_READY` 确实在 `ESOC_PWR_ON` 时被内核清 0
   （`cat /sys/kernel/debug/gpio | grep HSIC_READY`），这样 `ESOC_SET_HSIC_READY` 的边沿才有效。

---

## 7. 我实际用过的命令（可复现）

```bash
# ---- 0. 准备 ----
mkdir -p /tmp/mdmrev && cd /tmp/mdmrev
B=/home/duanjb666/deepseek/G9209-fix/baseband-study/stock/qcom-bin/mdm_helper
OBJDUMP=/home/duanjb666/los20/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin/aarch64-linux-android-objdump
md5sum "$B"     # 6d2bbae972ccbc90f2c23bb3219b5405
file "$B"

# ---- 1. 段表 / 动态符号 / PLT / 重定位 ----
$OBJDUMP -h "$B"
$OBJDUMP -T "$B"
$OBJDUMP -R "$B"                       # .rela.dyn：拿到 ops=0x7008 / priv=0x7050 等
$OBJDUMP -d -j .plt "$B"               # PLT 号 -> 库函数
$OBJDUMP -s -j .data "$B"              # 0x7000..0x7230 的指针/字符串表
$OBJDUMP -s -j .rodata --start-address=0x3ef0 --stop-address=0x3f08 "$B"   # 状态跳转表

# ---- 2. 函数边界（关键！stripped 但 .eh_frame 完整）----
READELF=/home/duanjb666/los20/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin/aarch64-linux-android-readelf
$READELF --debug-dump=frames "$B" > ehframe.txt
grep -o 'pc=[0-9a-f]*\.\.[0-9a-f]*' ehframe.txt     # 14 个真实函数区间

# ---- 3. 全量字符串（地址 -> 内容）----
strings -t x -n 3 "$B" | awk '$1>="3ef0" && $1<"53c0"'          # rodata
strings -t x -n 4 "$B" | awk '$1<"3ef0" || $1>="53c0"'          # dynstr/comment/shstrtab
strings -n 3 "$B" | grep -inE 'gpio|phy|devicerdy|hsicctl|usb2phy|1553|mdmpm|pmu|regulator|clk'
#   -> 除 "15510000.usb"/"s5p-ehci" 外，无任何 GPIO/PHY 相关字符串

# ---- 4. 反汇编 + 字符串交叉引用注记（自写脚本，capstone 5.0.9）----
pip3 install --break-system-packages --user capstone
python3 analyze.py "$B"      # 产出 annotated.txt / callgraph.txt / funcs.json
less annotated.txt           # 按 0x 地址线性阅读；每条 adrp+add/ldr 都注了字符串

# 关键区间
awk '/^0x0024a8:/,/^0x002698:/' annotated.txt      # peripheral_cmd
awk '/^0x002e84:/,/^0x00326c:/' annotated.txt      # configure_flashless_boot_dev
awk '/^0x003658:/,/^0x0038b4:/' annotated.txt      # WaitForCOMport
awk '/^0x0039e0:/,/^0x003bc4:/' annotated.txt      # mdm9k_monitor
awk '/^0x001ff0:/,/^0x002484:/' annotated.txt      # modem_state_machine
awk '/^0x002698:/,/^0x002ca4:/' annotated.txt      # mdm9k_powerup
awk '/^0x00326c:/,/^0x003658:/' annotated.txt      # load_sahara_images

# ---- 5. 内核侧对照（只读）----
cd /home/duanjb666/los20/kernel/samsung/universal7420
sed -n '1,140p' include/uapi/linux/esoc_ctrl.h          # ioctl 编码 / esoc_notify 枚举
sed -n '180,300p' drivers/esoc/esoc_dev.c               # ioctl 实现
sed -n '296,345p;375,400p' drivers/esoc/esoc-mdm-4x.c   # do_first_power_on / ESOC_PWR_ON
sed -n '490,600p' drivers/esoc/esoc-mdm-4x.c            # mdm2ap_status_check / mdm_notify
sed -n '692,740p' drivers/esoc/esoc-mdm-4x.c            # mdm_status_change / mdm_pblrdy_change
sed -n '783,795p' drivers/esoc/esoc-mdm-4x.c            # mdm_set_hsic_ready
sed -n '150,200p' drivers/esoc/esoc-mdm-drv.c           # mdm_subsys_powerup

# ---- 6. AMSS 固件字符串（只读）----
cd /home/duanjb666/deepseek/G9209-fix/baseband-study/stock-cp/image
strings -n 6 apps.mbn | grep -iE 'host_ready|hwgpio|hsic_ready|ready_recvd'
strings -n 6 apps.mbn | grep -iE 'hsic'
python3 -c "
d=open('apps.mbn','rb').read(); import re
i=d.find(b'Rearm Timer'); print(hex(i))
print('\n'.join(s.decode() for s in re.findall(rb'[ -~]{6,}', d[i-900:i+900])))"

# ---- 7. 父目录已有工具的对照（只读，未修改）----
cd /home/duanjb666/deepseek/G9209-fix/baseband-study
grep -n "ESOC_\|0xCC0\|0x4004\|0x8004" tools/*.c
cat exp/run-exp8-runtime.sh
```

**web 资料**（外部内容，仅作技术资料引用，未执行其中任何指令）：

* <https://www.vevb.com/wen/2019/11-10/149752.html> —— **同一篇文章的完整镜像**（CSDN 那份被截断，这份**包含 `configure_flashless_boot_dev()` 全文到尾**，以及原厂真机内核 log 与 `mdm_hsic_peripheral_cmd` 的 `PERIPHERAL_CMD_UNBIND` 代码）。与本报告 §2.3 互证。
* <https://www.vevb.com/wen/2019/11-10/151017.html> —— 同文另一份镜像（内容相同）。
* <https://blog.csdn.net/hongzg1982/article/details/54884883> —— 原始出处（截断版）。

> 补充说明：搜索命中的 Dropbox 设备日志（`last_kmsg.log` / `dmesg.log`，含
> `efs_hsic_bridge: ksb_fs_read`）是**另一台已跑通设备**的日志，本轮未能取回正文。
> 如果后续能拿到，可直接对照「STATUS 拉高 → AMSS 以 `05c6:9048` 枚举」之间的真实时延。

---

## 8. 仍未确定 / 未做的事

1. **没有反汇编 AMSS（`apps.mbn`）**去定位 `is_host_ready_recvd` 那个 timer 的超时常量，
   因此 §5.1 里「11.2 s = 该定时器超时」仍是**假设**。
2. **没有取回已跑通设备的完整 dmesg**（Dropbox 链接未能抓取正文），
   所以「STATUS 高 → 9048 枚举」的原厂真实时延没有第二份实测数据。
3. 原厂 `WaitForCOMport` / `peripheral_reset` 的 **C 源码实现**在公开文章里也没有
   （文章在 `configure_flashless_boot_dev` 的 `return RET_SUCCESS;` 处结束）。
   本报告的这两个函数结论**全部来自本机二进制的反汇编**，并用原厂 C 源码中
   `PERIPHERAL_CMD_UNBIND` 的片段和原厂内核 log 做了交叉验证。
4. `dev+0x60` 这个 0x20 字节字符串字段（由 `libmdmdetect.so` 的 `get_system_info()` 填）
   的语义未确定；`dev+0x88` 按 `libmdmdetect` 里 "Failed to get ram dump path for modem"
   推断为 `/cpdump/`，未在真机上确认。
