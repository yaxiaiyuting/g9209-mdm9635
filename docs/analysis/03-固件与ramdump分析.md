# FINDINGS — Modem 11.2 秒自复位的固件 / dump 证据

> 纯离线二进制分析报告。**未使用 adb，未接触真机，未修改 `baseband-study/` 下任何已有文件。**
> 分析对象：SM-G9209 外挂 MDM9635（`9x35A-AAAHANAZA-40000000` / `MPSS.BO.2.0.1.c3.5-00221-M9635TAARANAZM-1`）的原厂 CP 包与既有 dump。
> 生成日期：2026-10-03

---

## 0. 结论速览（TL;DR）

| # | 结论 | 强度 |
|---|---|---|
| 1 | **`RST_STAT.BIN`（ResetStatusRegion）就是 `0x07fb64d8` 处 4 字节，描述名 "Reset Status Region"，实测 `02 00 00 00`**。此地址由**两个互相独立的来源**确证：modem 自己的 dump 区描述表 + 随 dump 一起抓下来的 `load.cmm`。 | **确证** |
| 2 | `PmicPONstat` 是 `0x07fb64e0` 处 **8 字节**（`PMIC_PON.BIN`），实测 `20 00 02 00 02 00 00 00`。它与 RST_STAT 紧邻，同属 modem **SMEM**（SMEM base = `0x07f00000`，由 `sbl1.mbn` 中的立即数确证）。奇数位全为 0 → 字段宽度是 **16 位或 32 位**，即这是 **2~4 个寄存器**，不是 8 个字节寄存器。 | **确证**（地址/长度/取值）/ **推测**（位含义） |
| 3 | **`MDM_ERR_FATAL.BIN`（4224 字节，`0x011146d8`）内容全部为 `0x00`**（`mdm_err.log` / `mdm_err.txt` 三份文件 md5 完全相同：`aa056ba75bb5f4bbe4949b8a05190ef8`）。**没有任何字符串、错误码、模块名、时间戳、崩溃签名**——一个字节都没写过。 | **确证** |
| 4 | `.mbn` 固件里**不存在** `0x9048 / 0x904C / 0x9075` 这三个 USB PID（既不是 MOVW 立即数，也不是 USB 描述符字节）。**唯一存在的 VID/PID 是 `05c6:9008`，并且它在 `sbl1.mbn` 里**（设备描述符 `12 01 00 02 00 00 00 40 c6 05 08 90 …`，文件偏移 `0x2a13c`）。AMSS 侧镜像（`qdsp6sw.mbn`/`apps.mbn`）**没有任何 USB 设备描述符**。 | **确证** |
| 5 | `ks.real`（原厂 kickstart）**在"12 个镜像传完 + `DONE_RESP(mode=1)` 之后没有任何后续启动动作**，直接 `Successfully uploaded all images` → 返回 1 → 退出。`memdump` / `ramdumpimage` / `commandop.bin` / `efssyncloop` **全部是命令行开关控制的"可选 dump 路径"**，正常启动路径不会走到。它**确实**会发 `ESOC_SET_CRASH`（ioctl `0x4004cc09`），但只在 dump 路径里。 | **确证** |
| 6 | 原厂 AP 侧流程（`mdm_helper`）在**镜像传完之后还有 4 个步骤**：HSIC bind → 等 `MDM2AP_STATUS` 拉高 → **再跑一次 ks（`-l` efssyncloop）做 EFS sync**（走 `/dev/efs_hsic_bridge`）→ 发 `ESOC_BOOT_DONE` → 发 `ESOC_DIAG_DISABLE`。当前用户的流程**这 4 步一个都没做**。 | **确证**（字符串 + 内核源码）|
| 7 | **11.2 秒复位不是 AMSS 的 ERR_FATAL / 断言路径**：err_fatal 区全 0 + `MDM2AP_ERRFATAL` 全程 lo + 没有 `ESOC_ERR_FATAL` 事件，三者互相印证。 | **排除（强）** |
| 8 | 最可能：**modem 侧的看门狗（dog bite）或一条不经过 err_fatal 的内部/硬件复位**。依据：复位被 bootloader 记录（RST_STAT ≠ 0）却**没有任何软件故障记录**，且时间极其稳定（11.19/11.18/11.20 s）。 | **推测（中等偏强）** |
| 9 | 次可能：**AMSS 等 AP 侧完成启动握手（HSIC/MHI/EFS-sync/ESOC_BOOT_DONE）超时后自己复位**。依据：原厂流程后 4 步全缺 + `/dev/efs_hsic_bridge` 从未出现 + AMSS 内确有大量 SIO/MHI/HSIC 超时逻辑。 | **推测（中等）** |
| 10 | `ResetStatusRegion = 2` **具体对应哪一种复位源：未定**。整个 CP 包（9 个 `.mbn`，含 56 MB 的 `qdsp6sw.mbn`）+ SBL1 + DDRCS0 里**不存在任何 reset-reason 字符串或枚举表**（无 `PON_WARM_RESET` / `WDOG` / `PS_HOLD` / `RST_STAT` 之类的名字）。 | **未定** |

---

## 1. 素材清单与校验

```
$ cd /home/duanjb666/deepseek/G9209-fix/baseband-study/
$ md5sum evidence/cpdump/*.BIN mdm-crash/mdm_err.*
5ffb2f41cbcc1ee70b0c9c01428bd250  CMMScript.BIN       (1212 B)
aa056ba75bb5f4bbe4949b8a05190ef8  MDM_ERR_FATAL.BIN   (4224 B)
243ab86969da68af2717dbda1cd33909  PmicPONstat.BIN     (8 B)
f2dd0dedb2c260419ece4a9e03b2e828  ResetStatusRegion.BIN (4 B)
aa056ba75bb5f4bbe4949b8a05190ef8  mdm_err.log
aa056ba75bb5f4bbe4949b8a05190ef8  mdm_err.txt
```

`stock-cp/image/` 内 9 个原厂镜像（本次分析全部用到）：

| 文件 | 大小 | 格式 | 说明 |
|---|---|---|---|
| `sbl1.mbn` | 182584 | 私有 MBN 头（非 ELF） | 二级引导，**含 dump 区描述表** |
| `tz.mbn` | 336248 | ELF32 ARM | TrustZone |
| `rpm.mbn` | 158284 | ELF32 ARM | 资源电源管理 |
| `sdi.mbn` | 20564 | ELF32 ARM | |
| `mba.mbn` | 371792 | ELF32 ARM | Modem Boot Authenticator |
| `qdsp6sw.mbn` | 56079232 | ELF32 ARM (5 段) | **Q6/AMSS 主体** |
| `dsp2.mbn` | 5147988 | ELF32 | ADSP |
| `apps.mbn` | 3359376 | ELF32 ARM | **APPS(ARM) 侧：OKL4 微内核 + AMSS + `sys_m`** |
| `acdb.mbn` | 2097192 | — | 音频校准库 |

> ⚠️ 重要更正：本报告用事实推翻了"AMSS 主体 = `qdsp6sw.mbn`"的直觉假设。**`apps.mbn` 才是跑 ARM 代码、含 OKL4 微内核、含 `sys_m` 系统监视任务、并且真的被加载进 DDR 的那一个镜像**（见 §3.5 的 DDRCS0 对应关系）。`qdsp6sw.mbn` 是 Q6 DSP 侧固件。

`sbl1.mbn` 头部解析（文件偏移 `0x00`–`0x4f`，代码从 `0x50` 开始）：

```
00000000: d1dc 4b84 3410 d773 1500 0000 ffff ffff   magic / image_id=0x15=21 ✓
00000010: ffff ffff 5000 0000 0040 00f8 e8c8 0200   hdr_len=0x50 load=0xF8004000 size=0x2C8E8
00000020: e8af 0200 e8ef 02f8 0001 0000 e8f0 02f8   seg2 ...
```
→ `sbl1.mbn` 的代码基址 = **`0xF8004000`**，文件偏移 `X` 对应虚拟地址 `0xF8004000 + (X - 0x50)`。
（image_id 21 与简报里 Sahara 的第一条镜像 `21 sbl1` 完全吻合。）

---

## A. `ResetStatusRegion = 2` 是什么含义？

### A.1 地址与含义：来自 modem 自己的"崩溃 dump 区描述表"（**确证**）

这批 `cpdump/*.BIN` 不是随便读的——modem 在 SMEM 里维护了一张 **dump 区描述表**，主机会读它、按它去 dump。我找到了这张表的**两份独立副本**：

**副本 1：`evidence/18-mem-dump-832.bin`（832 字节 = 16 项 × 52 字节，二进制原表）**

```
$ python3 -c "..."   # 完整命令见 §E
entry = { u32 enabled; u32 addr; u32 size; char desc[20]; char filename[16]; u32 pad }  = 52 B

 #  en      address       size  filename       desc
 0   1 0x00fe800000 0x00008800  OCIMEM.BIN     OCIMEM
 1   1 0x00fc100000 0x00020000  CODERAM.BIN    RPM Code RAM region
 2   1 0x00fc190000 0x00010000  DATARAM.BIN    RPM Data RAM region
 3   1 0x00fc428000 0x00004000  MSGRAM.BIN     RPM MSG RAM region
 4   1 0x00fe100000 0x00004000  LPM.BIN        LPASS LPM region
 5   1 0x00fd4c0000 0x00018ffc  IPA_REG1.BIN   IPA 1 region
 6   1 0x00fd4e0000 0x00008ffc  IPA_REG2.BIN   IPA 2 region
 7   1 0x00fd4f8000 0x000020fc  IPA_REG3.BIN   IPA 3 region
 8   1 0x00fd4f0000 0x00001000  IPA_IRAM.BIN   IPA uC iram region
 9   1 0x00fd4f4000 0x00001f00  IPA_DRAM.BIN   IPA uC dram region
10   1 0x0007fb64e0 0x00000008  PMIC_PON.BIN   Pmic PON stat          <<<<<
11   1 0x0007fb64d8 0x00000004  RST_STAT.BIN   Reset Status Region    <<<<<
12   1 0x0000000000 0x08000000  DDRCS0.BIN      DDR CS0 Memory
13   1 0x00011146d8 0x00001080  mdm_err.log    MDM_ERR_FATAL
14   1 0x00011146d8 0x00001080  mdm_err.txt    MDM_ERR_FATAL
15   2 0x0007fa9320 0x000004bc  load.cmm       CMM Script
```

**副本 2：`evidence/cpdump/CMMScript.BIN`（1212 字节，= 表里第 15 项的 `load.cmm` 本身，从 `0x07fa9320` 读出来的）**

```
; Build ID: 9x35A-AAAHANAZA-40000000
; Time Stamp unavailable from boot loader.
...
if OS.FILE(PMIC_PON.BIN)
(
  d.load.binary PMIC_PON.BIN 0x7fb64e0 /noclear
)
if OS.FILE(RST_STAT.BIN)
(
  d.load.binary RST_STAT.BIN 0x7fb64d8 /noclear
)
if OS.FILE(DDRCS0.BIN)
(
  d.load.binary DDRCS0.BIN 0x0 /noclear
)
if OS.FILE(mdm_err.log)
(
  d.load.binary mdm_err.log 0x11146d8 /noclear
)
```

→ 地址 `0x07fb64d8`（= `0x7fb64d8`）在两处**逐字节一致**，长度 **4 字节**，文件名 **`RST_STAT.BIN`**，人类可读描述 **"Reset Status Region"（复位状态区）**。

**副本 3（第三重确证）：`stock-cp/image/sbl1.mbn` 里也内嵌了这张表的字符串池**

```
$ xxd -s 0x104b0 -l 240 stock-cp/image/sbl1.mbn
000104f0: e064 fb07 504d 4943 5f50 4f4e 2e42 494e  .d..PMIC_PON.BIN
00010500: 0000 0000 506d 6963 2050 4f4e 2073 7461  ....Pmic PON sta
00010510: 7400 0000 5253 545f 5354 4154 2e42 494e  t...RST_STAT.BIN
00010520: 0000 0000 5265 7365 7420 5374 6174 7573  ....Reset Status
00010530: 2052 6567 696f 6e00 4444 5243 5330 2e42   Region.DDRCS0.B
00010540: 494e 0000 2044 4452 2043 5330 204d 656d  IN.. DDR CS0 Mem
00010550: 6f7279 0044 4452 4353 312e 4249 4e00 0000  ory.DDRCS1.BIN...
00010560: 2044 4452 2043 5331 204d 656d 6f72 7900   DDR CS1 Memory.
00010570: 0000 0000 6d64 6d5f 6572 722e 6c6f 6700  ....mdm_err.log.
00010580: 4d44 4d5f 4552 525f 4641 5441 4c00 0000  MDM_ERR_FATAL...
00010590: 6d64 6d5f 6572 722e 7478 7400 c801 f007  mdm_err.txt.....
```

即：**这张 dump 表是 modem 自己的引导链（SBL1）生成的**，`0x07fb64d8` 就是 SBL1 认定的"复位状态"存放点。

`stock-cp/image/sbl1.mbn` 精确偏移：`0x104f0`（`PMIC_PON` 地址字）、`0x104f4`（"PMIC_PON.BIN"）、`0x10504`（"Pmic PON stat"）、`0x10514`（"RST_STAT.BIN"）、`0x10524`（"Reset Status Region"）。

**这三个文件在生产端的关系**（`stock/init.baseband.rc`，Samsung 原厂）也印证了它的用途：

```
78:    copy /cpdump/PMIC_PON.BIN /tombstones/mdm/PMIC_PON.BIN
79:    copy /cpdump/RST_STAT.BIN  /tombstones/mdm/RST_STAT.BIN
```

→ 原厂固件在 modem 崩溃后会把这两个区抓下来存到 `/tombstones/mdm/`。**它就是"复位原因记录"**，只是原厂把它当黑盒存档，没有在可执行代码里解释它。

### A.2 实测取值

```
$ xxd evidence/cpdump/ResetStatusRegion.BIN
00000000: 0200 0000                                ....          → u32 LE = 2

$ xxd evidence/cpdump/PmicPONstat.BIN
00000000: 2000 0200 0200 0000                       .......       → 8 字节
```

### A.3 `PmicPONstat` 的位含义（**推测**，附推理过程）

`20 00 02 00 02 00 00 00` 的关键结构特征是**所有奇数位置都是 0x00**：

```
按 u8  : 20 00 02 00 02 00 00 00
按 u16 : 0x0020, 0x0002, 0x0002, 0x0000
按 u32 : 0x00020020, 0x00000002
```

如果是"连续 8 个 8 位 PMIC PON 寄存器"，偶数位置不可能全 0。所以字段宽度必然是 **16 位或 32 位**（PMIC 寄存器本身是 8 位，读出来存进 u16/u32 是 Qualcomm 的常见做法）。

于是：

| 偏移 | u16 解释 | u32 解释 | 合理寄存器名（**推测**） | 取值 | 置位 |
|---|---|---|---|---|---|
| `+0x00` | `0x0020` | `0x00020020` | `PON_REASON1` | 0x20 | **bit5** |
| `+0x02` | `0x0002` | ↑ | `PON_REASON2` | 0x02 | **bit1** |
| `+0x04` | `0x0002` | `0x00000002` | `PON_FAULT_REASON1` | 0x02 | **bit1** |
| `+0x06` | `0x0000` | ↑ | `PON_FAULT_REASON2` | 0 | — |

**能确证的部分**：
1. `PON_REASON` 非 0 ⇒ 这不是一次"纯冷启动/首次上电"，PMIC 里**留有明确的触发源位**。
2. `PON_FAULT_REASON` 也有非 0 位 ⇒ PMIC 记录到了一次**异常/强制**掉电或复位事件（不是正常的 KPDPWR 按键关机）。
3. 两个 reason 寄存器**同时**有位置 1，说明这是一次"被记录的、有来源的"复位——与"modem 自己把 MDM2AP_STATUS 拉低然后复位"完全一致。

**不能确证的部分（诚实声明）**：我没有在本地找到 PMD9635/PM8994 系列 PON 外设的**位定义表**，因此 `bit5 = ?`、`bit1 = ?` 无法在本地证据下钉死。社区常见的 PM8x41 家族定义为：

```
PON_REASON1 : bit0 KPDPWR | bit1 RESIN | bit2 KPDPWR_RESIN | bit3 USB_CHG
              bit4 DC_CHG | bit5 PON1  | bit6 PON2          | bit7 (rsvd)
```
按此表 `0x20` = **PON1**（一个通用外部触发脚），`0x02` = **RESIN**。**但这是外部资料，不是本机证据，标记为未证实**，不应作为结论使用。

### A.4 为什么"2"钉不死：固件里**没有**任何 reset-reason 枚举（**确证为"不存在"**）

我对全部 9 个 `.mbn` + DDRCS0（12 MB）做了穷尽式字符串搜索，关键词：`reset_reason` / `rst_reason` / `pon_reason` / `boot_reason` / `PON_WARM` / `PON_HARD` / `WDOG` / `watchdog` / `KPDPWR` / `PS_HOLD` / `warm_reset` / `hard_reset` / `Reset Reason` / `RST_STAT` / `reason`。

结果（去掉与复位无关的命中）：

```
######## sbl1.mbn     : 无
######## tz.mbn       : SPI WDOG NS Bite / SPI RPM WDOG Bite / RPM_WDOG / FpGSPI WDOG NS Bite / SPI WDog Bark
######## rpm.mbn      : 无
######## mba.mbn      : 无
######## sdi.mbn      : 无
######## acdb.mbn     : 无
######## apps.mbn     : Watchdog detects task starvation %d / Watchdog startup timeout / Dog bark timeout.
                        q6_wdog_expired_irq / rpm_wdog_expired_irq / mpm_secure_wdog_bark_irq / sys_m_wdog_reset_isr
                        ssr:return:MDM-LPASS:Wdog bite / ssr:return:MDM-Q6SW:Wdog bite
                        LPASS wdog bite / Modem wdog bite / LPASS error fatal / MPSS error fatal
######## qdsp6sw.mbn  : HW WATCHDOG MENU / SW WATCHDOG MENU / [2] SW WATCHDOG / [3] HW WATCHDOG
                        q6_wdog_irq / MPM2_WDOG / MPM2_PSHOLD / SFR Init: wdog or kernel error suspected.
######## dsp2.mbn     : MPM2_WDOG / MPM2_PSHOLD / q6ss_wdog_irq / WatchDog is not initilized for WDT_trigger_BITE()!
```

**`sbl1.mbn`（写这个区的那一级）连一个 reset-reason 名字都没有。** 所以：

* `RST_STAT` 是一个**裸整数**（或硬件寄存器快照），不是可读枚举；
* 想从固件文本里推出"2 = 哪个源"这条路**在本机素材上是死的**——这不是我没找到，而是**它不存在**。

**唯一带名字的复位源表**是 `qdsp6sw.mbn` 里的 MPM2 寄存器名表（文件偏移 `0x2369138`–`0x23691c4`，属于 HWIO 寄存器名表而非复位原因枚举）：

```
2369138 MPM2_G_CTRL_CNTR
2369149 MPM2_G_RD_CNTR
2369158 MPM2_SLP_CNTR
2369166 MPM2_QTIMR_AC
2369174 MPM2_QTIMR_V1
2369182 MPM2_TSYNC
236918d MPM2_APU
2369196 MPM2_TSENS
23691a1 MPM2_TSENS_TM
23691af MPM2_WDOG          <-- MSM Power Manager 的看门狗
23691b9 MPM2_PSHOLD         <-- PS_HOLD（掉电/复位）
```

`MPM2_WDOG` 与 `MPM2_PSHOLD` 是 MDM9x35 上**唯一两个能产生"整芯片复位"的硬件源**。如果 `RST_STAT = 2` 是这两个之一的编号，那么 **2 极可能落在 WDOG/PSHOLD 这一对里**——这与 §0 第 8 条的判断方向一致。**但"2 到底是不是 WDOG"仍然是推测，不是确证。**

### A.5 一个重要提醒：**这份 `RST_STAT.BIN` 是"复位之前"还是"复位之后"抓的？**

`evidence/cpdump/*.BIN` 与 `mdm-crash/` 是不同批次抓的（`18-mem-dump-832.bin` 19:46、`cpdump/` 21:54）。SBL1 在**每次**启动时都会重写这个区，所以：

* 若采样发生在某次 11.2 s 复位**之后** → `2` **就是那一次复位的分类**；
* 若采样发生在 AMSS 运行中（复位前）→ `2` 描述的是**上一次**复位（很可能是 `powerup` 那次 PS_HOLD 上电）。

**这个不确定性必须在下一轮实验里消掉**（见 §F 的判决性实验）。

---

## B. `MDM_ERR_FATAL.BIN` / `mdm_err.txt` 里有什么？——**什么都没有**

### B.1 内容：全 0（**确证**）

```
$ python3 -c "d=open('evidence/cpdump/MDM_ERR_FATAL.BIN','rb').read(); print(len(d), sum(1 for b in d if b))"
4224 0
```

* 长度 4224 = `0x1080`，与描述表第 13/14 项的 `size` 字段**逐位吻合**。
* **非零字节数 = 0**，运行长度扫描「nonzero runs: 0」。
* `strings -a -n 4` 输出为空。
* `mdm_err.log`、`mdm_err.txt`、`MDM_ERR_FATAL.BIN` 三个文件 **md5 全同**（`aa056ba7…`）——即它们是同一个 4224 字节区的三次拷贝。
* ⚠️ **更正一处容易犯的错**：`0x011146d8` 是一个 **modem RAM 地址**，不能用它去索引 70 MB 的 CP 容器文件。我验证过 `img/cp-stock-modem.bin[0x11146d8:0x11146d8+4224]`（3627 个非零字节）——那只是容器里**无关的数据**，不能拿来说"镜像里也是 0"。本节的结论**只**建立在 dump 出来的 4224 字节本身（确实全 0）之上。

### B.2 判读

| 你要求找的线索 | 结果 |
|---|---|
| watchdog / WDOG / bark / bite | **不存在**（全 0） |
| AP_MDM handshake / HSIC / QMI | **不存在**（全 0） |
| boot / 模块名 / 时间戳 / 崩溃签名 | **不存在**（全 0） |
| 错误码 | **不存在**（全 0） |

**这是一个强否定证据，不是"没找到"。** `mdm_err` 是 SBL1/AMSS 在**致命错误**路径上写的那块记录区（描述名就叫 `MDM_ERR_FATAL`）。它**一个字节都没被写过**，与"`MDM2AP_ERRFATAL` GPIO 全程 lo"是同一个事实的两个侧面：

> **modem 在整个 11.2 秒里从未进入 "ERR_FATAL / 致命错误上报" 路径。**

⚠️ 一个必须说明的边界：**全 0 只能证明"没有任何一方写过致命错误记录"**，不能证明"没有发生任何错误"。看门狗 bite 属于硬件复位，软件通常**来不及**写这块区——这正是下一节判断的出发点。

---

## C. `qdsp6sw.mbn`（56 MB）与 AMSS 侧的证据

### C.1 版本 / 构建信息（**确证**）

```
qdsp6sw.mbn:
  0x1bd86c8  MPSS.BO.2.0.1.c3.5-00221-M9635TAARANAZM-1.154625.1.157564.1
  0x2fd5eb   /local/mnt/workspace/CRMBuilds/MPSS.BO.2.0.1.c3.5-00221-M9635TAARANAZM-1_20170322_234535/b/...
  0x1b9aaa0  SRCH_LIBRARY:9x35-TAARANAZQ-00221-Mar 22 2017-23:58:29
  0x1be8a10  MDM9635M
  0x1bef6c9  9x35 9/12/2014

dsp2.mbn:
  0x4094     M9x35AAAAAAAAQ1234.pbn
  0x38999f   M9x35AAAAAAAAQ1234_reloc
  0x38d938   QCOM time:Q6_BUILD_TS_Wed_Feb_03_22:50:22_PST_2016_ADSP.BF.2.4.5.c3-00033-M9635
  0x38d988   ENGG time:Q6_BUILD_TS_Tue_Sep_18_12:08:42_PST_2018_MDM9X35_0x8fffffff
  0x3a11fd   MDM9635M

apps.mbn (OKL4 微内核, debug 版):
  ~0x1c4ef   Assert failure: '...' at build_9x35/kernel_okl4extras_debug/object/kernel/src/objmanager/objmanager.h:908
  ~0x1e2b8   panic(arch/armv7/kernel/kernel_standard/src/validate.c:117) : Supported Cache operation is not ...

evidence/cpdump/CMMScript.BIN:
  ; Build ID: 9x35A-AAAHANAZA-40000000
```

> ⚠️ **`apps.mbn` / DDRCS0 里成百上千条 `Assert failure:` / `l4_ensure failure:` / `panic(...)` 是镜像里内嵌的断言字符串常量，不是"断言真的触发了"。** 这只能证明镜像带 debug 断言（OKL4 debug kernel），**不能**作为崩溃证据。

### C.2 `0x9048 / 0x904C / 0x9075` 是否出现在固件里？——**完全不出现**（**确证**）

三重搜索，全部为负：

**(1) 文本搜索**（`strings -n 5` + 正则 `\b(9048|904c|9075)\b`）：9 个镜像 + DDRCS0 **全部 0 命中**。（`grep -i 9048` 的少数命中都是文件名子串，如 `lte_ml1_mgr_task.c`、`npa_graph.c`，与 PID 无关。）

**(2) Thumb-2 / ARM `MOVW` 立即数搜索**（`movw rX,#0x9048` 的两种编码，16 个目标寄存器全枚举）：

```
apps.mbn         0x9008: ['0x1c25a', '0x1c2c2']     <- 只有 0x9008 命中
dsp2.mbn         0x9008: ['0x145c1c']
sbl1.mbn         0x9008: ['0x198be']
DDRCS0.BIN       0x9008: ['0x1925a', '0x192c2']
0x9048 / 0x904c / 0x9075 : 全部镜像 0 命中
```

**(3) USB 设备描述符字节搜索**（`idVendor=0x05c6` 小端 = `c6 05`，紧跟 idProduct 小端）：

```
05c6:9008 (PBL/Sahara QDL) : sbl1.mbn @ 0x2a144 ; img/cp-stock-modem.bin @ 0x581144 ; radio-current.img @ 0x581144
05c6:9048                  : 无
05c6:904c                  : 无
05c6:9075                  : 无
05c6:901d / 05c6:900e / 05c6:9006 : 无
```

**唯一的完整 USB 设备描述符在 `sbl1.mbn`**：

```
$ xxd -s 0x2a120 -l 96 stock-cp/image/sbl1.mbn
0002a130: 0000 0000 0000 0000 0000 0000 1201 0002  ................
0002a140: 0000 0040 c605 0890 0000 0102 0001 0902  ...@............
0002a150: 2000 0101 0080 0109 0400 0002 ffff ff00   ...............
0002a160: 0705 8102 4000 0007 0501 0240 0000 0902  ....@......@....
```
解析：`12`=bLength 18，`01`=DEVICE，`00 02`=bcdUSB 0x0200，`00`=class，`00`=subclass，`00`=proto，`40`=bMaxPacketSize0，**`c6 05`=idVendor 0x05C6，`08 90`=idProduct 0x9008**。
→ **SBL1 跑起来之后仍然把 USB 重新枚举成 `05c6:9008`。**

补充（避免误判）：`apps.mbn` 里也有一个 `12 01 00 02 …` 命中（文件偏移 `0x2f8108`），但解析出来是 **`idVendor=0x0000, idProduct=0x0000`** 的**空模板**，不是可用的设备描述符：
```
12 01 00 02 00 00 00 08 00 00 00 00 00 00 00 00 00 00
bLength 18  type 1  bcdUSB 0x0200  bMaxPacket 8  idVendor 0x0  idProduct 0x0
```
（`qdsp6sw.mbn` @ `0x161e120` 的命中落在数据区，同样不是描述符。）

**结论（回答 C 的核心问题）**：
> 这个平台的 AMSS 阶段**根本不用新的 USB PID**。`0x9048/0x904C/0x9075` 在这套固件里**不存在**，所以"等一个 AMSS 阶段的 USB PID 出现"这条路在原厂固件下**本来就不会发生**。观测到 USB 一直停在 `05c6:9008` 是**预期行为，不是故障**。
> 主机与 modem 启动后的数据通路是 **HSIC + MHI/SMD/QMI**（`apps.mbn` 里 `mhi_*` 相关字符串 1354 条、`qmi*` 693 条、`hsic` 110 条），而不是一次新的 USB 枚举。

相关旁证（`apps.mbn` 精确偏移）：
```
0x60c9c  - sys_m_sio_open(SIO_PORT_MHI_SSR)
0x60cc4  - sys_m_sio_open(SIO_PORT_USB_SER3)
0x93554  mhi_sio_seri_close: port is NULL
0xa2f58  gcc_usb_hsic_clk
0xa2f70  VDD_USB_HS_HSIC
0xa30bc  - make the HSIC connect signal
0x2552a4 Sys_m hsusb notification reg failed
```

### C.3 AMSS 是否"等 AP 应答"？—— 有大量超时逻辑，但**没有一条写着"等 AP 11 秒"**（**推测**）

`apps.mbn` 里与"等待/超时/握手"相关的字符串（精确偏移）：

```
0x0bfeac  mhi_core_link_completion_timer_cb: Timeout            <- MHI 链路建立超时
0x276dea  hsu_lpm_hsic_timeout_tmr_cb()                         <- HSIC LPM 超时
0x276e08  HSU LPM HSIC timed out.
0x1313a0   - HSU LPM HSIC timed out.
0x25571a  sys_m: SSR notification ACK fail                      <- sys_m 等 SSR 通知 ACK 失败
0x26a5b2  Timeout occured waiting for data
0x26aa6b  Error in remotefs_sio_flush_tx: Timeout during sio_flush_tx   <- EFS/remote-fs 超时
0x2568c8  Diag timed out on SIO callback %d
0x1eca90  err_fatal_mproc_timeout_value
0x1f6004 / 0x25dc6d  Try to wakeup the host - wait for resume from AP until CONFIG_REMOTE_WAKEUP_TIMEOUT
0x2625f6  /nv/item_files/hsusb/ecm_rx_timeout_ms
0x2626df  /nv/item_files/hsusb/gpio_remote_wakeup_timeout
0x166104 / 0x26a4db  - Ignore the efs_sync!                     <- EFS sync 可以被忽略
```

**判读**：AMSS 侧确实存在"等 host/AP"的超时路径（MHI link completion、HSIC LPM、SIO、Diag、remote-fs），所以"等 AP 超时后自复位"在机制上是**可能的**；但我**没有找到任何常量或日志能对上 11.2 秒**，所以这条只能算**中等强度的推测**。

### C.4 看门狗 / 断言 / ERR_FATAL 机制（**确证存在这些机制**）

`apps.mbn` 精确偏移（`sys_m` = System Monitor 任务，正是拉起 `MDM2AP_STATUS` 的那个任务）：

```
0x607e0  sys_m
0x60804  ssr:ack
0x60814  ssr:return:MDM-LPASS:Wdog bite
0x60838  ssr:return:MDM-Q6SW:Wdog bite
0x60d20  ssr:return:
0x60da4  Set MDM2AP_STATUS_OUT_GPIO high
0x60dc8  MDM2AP_STATUS_OUT_GPIO go high
0x61210  ssr:return:MDM-LPASS:
0x6124c  ssr:return:MDM-MPSS:
0x61284  sys_mon
0x616b4  Rcived a SYS_M_SYSTEM_DIAG_DISABLED msg from sysmon channel!
0x61734  ssr:shutdown
0x617a0  system:reset
0x617d0  ssr:poweroff
0x61800  system:shutdown
0x61834  ssr:retrieve:sfr
0x25539e  - Ignore the SYS_M_AP2MDM_STATUS_GPIO low interrupt because it's not expected level.
0x2554e2  System monitor init with high AP2MDM_ERR_FATAL_GPIO
0x255516  System monitor init with low AP2MDM_STATUS_GPIO
0x2555c6  LPASS wdog bite
0x2555d6  Modem wdog bite
0x2556f7  LPASS error fatal
0x255709  MPSS error fatal
0x25571a  sys_m: SSR notification ACK fail
0x255921  Modem restarted due to an APQ error
0x2559b7  Modem restarted due to APQ restart
0x254e55  dog_get_report_period: Invalid ID.
0x254ed0  Dog bark timeout.
0x254efa  Watchdog detects task starvation %d
0x254f1e  Assertion index < DOG_NUM_TASKS failed
0x254f5e  Watchdog startup timeout
0x254f94  HALdog.c
0x254f9d  err_exception_handler.c
0x2a6e0b  ERR_FATAL Iter %d uid %d num_iter %d
0x23ca4c  err_fatal_action                 (NV: /nv/item_files/dnt/err/err_fatal_action)
0x23ca90  err_fatal_mproc_timeout_value
0x23cfb9  sys_m_wdog_reset_isr
0x602c0   ERR crash log report.  Version %d.
0x602ea   MDM-APSS crash report-
0x916a0   Dog Report Information (dog_state_table)
0x91cc8   Qualcomm, Incorporated
```

`qdsp6sw.mbn` 精确偏移：

```
0x1bd6d16  dog:hw_init_complete
0x1bd6d2b  dog_hb:ping                    <- 看门狗心跳
0x1bd6d37  Dog Report Information (dog_state_table)
0x1bd6d61   [idx]   Task Name   Pri  Timeout   Count  Is_Blocked
0x1bd6dbf  End Dog Report
0x1bd6dd0  SFR Init: wdog or kernel error suspected.       <-- ★ 见下
0x1bd6dfa  /nv/item_files/dnt/err/err_fatal_action
0x1bd7063  Exception
0x1bd7080  Error Fatal
0x1bd7113  cp_crash_history.txt
0x1bd7128  ERR_FATAL reentrancy violation, remove cb until resolved
0x1bd71e7  MDM-MPSS crash  report:-
0x300070   DELAY_ERR_FATAL_TIMER
0x23691af  MPM2_WDOG
0x23691b9  MPM2_PSHOLD
```

★ **`SFR Init: wdog or kernel error suspected.`（`qdsp6sw.mbn` @ `0x1bd6dd0`，`dsp2.mbn` @ `0x37f6e0`）是本次分析里最贴近答案的一条字符串**：它是 **SFR（failure-report 子系统）初始化**时打印的——意思是"**我在启动时读到：上一次是看门狗或内核错误导致的复位**"。这直接证明：

> **固件里存在一条"启动时读取上次复位原因 → 判定为 wdog/kernel error"的逻辑。** 也就是说这个平台**确实会把看门狗复位作为一个可识别的复位类别记录下来**，并且这个信息在下次启动时是**可读的**。

**进一步佐证 —— 固件里有一整套"错误/复位"可配置项**（`qdsp6sw.mbn`，精确偏移）：

```
0x1bd6dfa  /nv/item_files/dnt/err/err_fatal_action
0x1c0f020  /nv/item_files/dnt/err/err_hw_reset_detect_enable   <-- ★ "硬件复位检测"开关
0x1c1097a  /nv/item_files/conf/dnt_err0.conf
0x1c1099c  /nv/item_files/conf/dnt_err1.conf
0x1c109be  /nv/item_files/conf/dnt_err2.conf
```

`err_hw_reset_detect_enable` 的存在说明"**硬件复位检测**"在这个固件里是一个**一等公民功能**：系统会在启动时主动检测"上一次是不是硬件（看门狗/PS_HOLD）复位"。这与 §E.2 的判断方向一致，也再次说明 `RST_STAT` 那个区确实承载"复位来源"语义。

这条逻辑读的很可能就是 `RST_STAT` 那个区（或等价的硬件寄存器）——但我**没有在反汇编层面把这条链闭合**（`qdsp6sw.mbn` 是 56 MB 的 Q6 镜像，Hexagon 代码，缺少符号与地址映射；见 §E.5 的说明）。所以：**机制存在（确证）；这条字符串读的就是 `0x07fb64d8`（未定）。**

**顺带排除一个可能**：我把 `11200` / `11200000`（us）/ `0x2BC0` 作为 32 位小端常量在 `apps.mbn` 与 `qdsp6sw.mbn` 里搜过，**没有任何一处命中**。所以 11.2 s **不是**一个写在代码里的字面超时常量——它要么是 NV 配置出来的，要么是某个计数器/时钟推导出来的，要么与 11.2 s 本身不是整数关系（例如 11.18 s ≈ 某个 2 的幂分频）。

---

## D. `ks.real` 里"boot 之后还要做的事"

`ks.real` = 原厂 kickstart，ELF64 AArch64 PIE，39272 B，stripped。**它的全部字符串就足以把状态机还原出来**，我再用反汇编验证了关键分支。

### D.1 完整选项表（**确证**，从 `.rela.dyn` 的相对重定位还原 `struct option[]`）

```
$ /tmp/csvenv/bin/python a64s.py ...   # 见 §E.3
  entry@0x99d8 name='--help'          has_arg=0 flag=0x0 val=0x68 '-h'
  entry@0x99f8 name='--port'          has_arg=1 flag=0x0 val=0x70 '-p'
  entry@0x9a18 name='--verbose'       has_arg=1 flag=0x0 val=0x76 '-v'
  entry@0x9a38 name='--command'       has_arg=1 flag=0x0 val=0x63 '-c'
  entry@0x9a58 name='--memdump'       has_arg=0 flag=0x0 val=0x6d '-m'   <-- 可选
  entry@0x9a78 name='--image'         has_arg=0 flag=0x0 val=0x69 '-i'
  entry@0x9a98 name='--sahara'        has_arg=1 flag=0x0 val=0x73 '-s'
  entry@0x9ab8 name='--prefix'        has_arg=1 flag=0x0 val=0x67 '-g'
  entry@0x9ad8 name='--where'         has_arg=1 flag=0x0 val=0x77 '-w'
  entry@0x9af8 name='--ramdumpimage'  has_arg=1 flag=0x0 val=0x72 '-r'   <-- 可选
  entry@0x9b18 name='--efssyncloop'   has_arg=0 flag=0x0 val=0x6c '-l'   <-- 可选
  entry@0x9b38 name='--rxtimeout'     has_arg=1 flag=0x0 val=0x74 '-t'
  entry@0x9b58 name='--maxwrite'      has_arg=1 flag=0x0 val=0x6a '-j'
  entry@0x9b78 name='--addsearchpath' has_arg=1 flag=0x0 val=0x62 '-b'
```

`-m` / `-r` / `-l` **全部 `has_arg` 语义都是"命令行开关"**。用法文本（`.rodata` 精确偏移）：

```
0x7d1b  -m   --memdump      Force Sahara memory debug mode
0x7e73  -r   --ramdumpimage Image ID which must be transferred before forcing Sahara memory dump mode
0x7d6e  -q <img_id> --quitafter   Force kickstart to exit after transmitting img_id
0x7dd4  -c <command_id> --command Force Sahara command mode
```

**原厂自己给的调用范例**（`ks.real` 内嵌注释，精确偏移 `0x8122`–`0x8390`）把三个用途分得清清楚楚：

```
0x8122  CALLING: kickstart.exe to retrieve serial number
0x8153    kickstart.exe -r 21 -c 1 -w c:\temp\...\ -p \\.\COM19 -s 16:efs1.bin -s 17:efs2.bin -s 20:efs3.bin -b ...
0x8202  CALLING: kickstart.exe for image transfer
0x822c    kickstart.exe -r 21 -w c:\temp\<sn>\ -p \\.\COM19 -s 16:m9kefs1 -s 17:m9kefs2 -s 20:efs3.bin ...
0x82e6  CALLING: kickstart.exe for EFS sync
0x830a    kickstart.exe -l -a 32 -m -v -w c:\temp\<sn>\ -p \\.\COM21 -t -1 -g m9k1_ > efs_sync_messages.txt
```

→ **原厂在产线上把 kickstart 调用了 3 次**，第 3 次就是 `-l … -m`（efssyncloop + memdump）。**"12 个镜像传完"只是第 2 次调用的一部分。**

### D.2 正常启动路径：传完之后**什么都不做**（**确证**）

反汇编证据 —— `SAHARA_WAIT_DONE_RESP` 分支（vaddr `0x4180`–`0x41f0`）：

```
00004180:  adrp  x3, #0x7000
00004184:  mov   w4, wzr
00004188:  mov   w0, #1
0000418c:  mov   w2, #0x2c2
00004190:  mov   x1, x20
00004194:  add   x3, x3, #0x27b          ; "!@ SAHARA_WAIT_DONE_RESP recieved with SAHARA_MODE_IMAGE_TX_PENDING=0x%.8X"
...
000041b8:  bl    #0x2be4
000041bc:  ldr   w0, [x19, #0x28]
000041c0:  bl    #0x2b90
000041c4:  ldr   w4, [x23, #8]           ; w4 = DONE_RESP->mode
000041c8:  cbnz  w4, #0x5674             ; mode != 0  -> 后续
000041cc:  ...  "!@ Still More images to be uploaded, entering Hello wait state"
000041e4:  str   wzr, [x19, #0x18]       ; state = SAHARA_WAIT_HELLO (继续传下一个镜像)
000041ec:  b     #0x3d7c

00005674:  cmp   w4, #1                  ; mode == 1 ?
00005678:  b.ne  #0x57cc                 ; 否则 "Received unrecognized status"
0000567c:  adrp  x1, #0x6000
00005680:  adrp  x3, #0x7000
00005688:  add   x3, x3, #0x329          ; 0x7329 -> "\n\n!@ Successfully uploaded all images\n"
00005694:  bl    #0x210c                 ; log(level=3, ...)
00005698:  mov   w0, #1
0000569c:  b     #0x5ba0                 ; <-- return 1, 直接返回
```

配合 `0x7320` 处的原始字节：
```
00007320: 69 74 20 73 74 61 74 65 00 0a 0a 21 40 20 53 75  |it state|  \n\n!@ Su|
0x732b = "!@ Successfully uploaded all images\n"
```

→ **`DONE_RESP(mode=1)` 之后：打一行日志，`return 1`，结束。没有 reset、没有 sleep、没有再发包、没有等 AP、没有 EFS sync。**（这也与简报里"v40 之后日志里不再有 `ESOC_SET_CRASH_OCCURRENCE`"一致——那条调用本来就是 dump 路径的。）

### D.3 `efssyncloop` / `memdump` / `ramdumpimage` / `commandop.bin`：**全部可选**（**确证**）

**(a) `efssyncloop` 由 `-l` 开关控制，反汇编为证**（`main` 的尾部，`0x5ce0`–`0x5d50`）：

```
00005d0c:  adrp  x20, #0x7000
00005d10:  adrp  x21, #0x7000
00005d14:  add   x20, x20, #0x444        ; 0x7444 "sahara_main"
00005d18:  add   x21, x21, #0x48e        ; 0x748e "!@ Sahara protocol completed"
00005d1c:  and   w22, w19, #1            ; w19 = efssyncloop 标志位（来自 -l）
00005d20:  mov   w0, w22
00005d24:  bl    #0x3c7c                 ; sahara_start(...)
00005d28:  and   w8, w0, #1
00005d2c:  tbz   w8, #0, #0x5d74         ; 失败 -> 跳出
00005d30:  ...   "!@ Sahara protocol completed"
00005d44:  tbnz  w22, #0, #0x5d1c        ; ★ 只有 -l 置位才回到循环头再跑一遍
00005d48:  mov   w19, #1
00005d4c:  b     #0x5d94                 ; 不置位 -> 直接结束
```

→ `-l`（`--efssyncloop`）就是"把整个 Sahara 上传流程循环跑"。**不带 `-l` 时只跑一次就退出。**

**(b) memory-dump / ramdump 路径由 `-m` / `-r` 门控**：`-m` 的官方描述就是 `Force Sahara memory debug mode`；`-r` 是 `Image ID which must be transferred before forcing Sahara memory dump mode`。相关字符串只在 dump 状态里出现：

```
0x6cdf  !@ RECEIVED <-- SAHARA_MEMORY_DEBUG
0x6d03  !@ Using 64 bit RAM dump mode
0x6d21  !@ Memory Table Address: %lu, Memory Table Length: %lu
0x6e3b  !@ STATE <-- SAHARA_WAIT_MEMORY_TABLE
0x6efc  !@ Memory Debug table received
0x6fa3  !@ Base 0x%lX Len 0x%lX, '%s', '%s'     <- 打印 dump 区表（就是 §A.1 那张表！）
0x6fc7  mdm_err.log                              <- 表里的文件名
0x700e  !@ num_debug_entries=%i
0x71c5  !@ STATE <-- SAHARA_WAIT_MEMORY_REGION
0x710b  !@ Writing to disk
0x7209  !@ Successfully downloaded files from target
0x7237  debug.mdm.cpdump_done                    <- ★ 只有 dump 路径才会设这个 prop
0x724d  true
0x7c72  commandop.bin                            <- 只在 CMD_EXEC 路径写
0x7b6b  !@ SENDING --> SAHARA_CMD_EXEC           <- 由 -c 触发
```

**(c) `commandop.bin`** 只出现在 `SAHARA_CMD_EXEC_DATA` 分支（`0x4a94` 引用 `0x7c72`），而 CMD_EXEC 由 `-c <command_id>` 强制触发。

### D.4 `ks.real` 会不会主动复位 modem？——**会，但只在 dump 路径**（**确证**）

`ks.real` 确实使用 `ESOC_SET_CRASH` 和 `ESOC_PANIC`：

```
0x690b  /dev/esoc-0
0x6917  !@ Could not open device %s
0x6933  !@ Could not issue ioctl ESOC_PANIC
0x6fd3  !@ Could not issue ioctl ESOC_SET_CRASH
0x6ffb  !@ ESOC_SET_CRASH
0x7028  !@ NOT BREAKING, falling into case SAHARA_WAIT_MEMORY_REGION  *************
```

反汇编（vaddr `0x3fc4`–`0x401c`，位于 dump 区循环里，紧挨 `mdm_err.log` 处理与 `strcmp(fn, "mdm_err.log")`）：

```
00003fc0:  bl    #0x1320                 ; open(...)
00003fc4:  mov   w23, w0
00003fc8:  tbnz  w23, #0x1f, #0x4020    ; fd<0 -> "!@ Could not open device %s"
00003fcc:  mov   w1, #0x40040000
00003fd0:  movk  w1, #0xcc09             ; w1 = 0x4004CC09
00003fd4:  sub   x2, x29, #0x7c
00003fd8:  mov   w0, w23
00003fdc:  bl    #0x1510                 ; ioctl(fd, 0x4004CC09, &arg)
00003fe0:  tbz   w0, #0x1f, #0x3ffc
00003fe4:  ...   "!@ Could not issue ioctl ESOC_SET_CRASH"
00003ffc:  ...   "!@ ESOC_SET_CRASH"     ; level 4
00004014:  mov   w0, w23
00004018:  bl    #0x12f0                 ; close(fd)
```

**ioctl 号验证**（对着内核头文件算）：

```
$ sed -n '1,20p' include/uapi/linux/esoc_ctrl.h
#define ESOC_CODE   0xCC
#define ESOC_SET_CRASH  _IOW(ESOC_CODE, 9, u32)
$ python3 -c "v=0x4004cc09; print('dir',(v>>30)&3,'size',(v>>16)&0x3fff,'type',hex((v>>8)&0xff),'nr',hex(v&0xff))"
dir 1 size 4 type 0xcc nr 0x9          ->  _IOW(0xCC, 9, u32) == ESOC_SET_CRASH  ✓
```

→ **`ks.real` 确实会发 `ESOC_SET_CRASH`（ioctl `0x4004cc09`），但那段代码在"遍历 dump 区表"的循环里**（同一个循环里还 `strcmp(fn,"mdm_err.log")`、写盘、`debug.mdm.cpdump_done`），**正常启动路径根本走不到**。这就解释了 v39 自伤行为的来源：它是从 kickstart 的 **dump 路径**抄过来的，抄到了正常启动路径上。

**另外**：`ks.real` 里**没有** `restart`、`msm_subsys`、`reboot` 这些字符串——v39 里"写 `restart` 到 `/sys/kernel/debug/msm_subsys/esoc0`"那一步**不是**从 kickstart 抄的。

### D.5 它如何判断启动成功？等多久？（**确证**）

* **不等 AP**。成功判定 = 收到 `SAHARA_END_IMAGE_TX` → 发 `SAHARA_DONE` → 收到 `DONE_RESP` 且 `mode==1`。
* 超时只作用于**串口/USB 读**：`-t <rxtimeout>`（`--rxtimeout`）；`0x61ab "Timeout Occured, No response or command came from the target!"`，`0x68f3 "!@ HELLO PACKET TIMEOUT"`。
* 搜索路径默认含 `/firmware/images/`（`0x6355`）。
* 有 wake lock：`0x83c2 /sys/power/wake_lock`、`0x83d7 /sys/power/wake_unlock`。
* 退出时 `port_disconnect` / `Disconnecting from com port`。

**综合 D 的裁决**：
> `efssyncloop`（`-l`）、`memdump`（`-m`）、`ramdumpimage`（`-r`）、`commandop.bin`（`-c`）**全部是可选的、仅 dump / 产线 EFS 同步用途**，**不是启动必需**。
> **`ks.real` 在"12 个镜像 + `DONE_RESP(mode=1)`"之后的正常启动路径上，不会做任何额外动作，也不会复位 modem。**
> ⇒ **11.2 秒复位不是 kickstart 干的；`ks.real` 也无法解释它。** 这与简报里"v40 已移除自伤逻辑但 11.2 s 依旧"完全自洽。

---

## E. 对"11.2 秒复位"的判断

### E.1 三条被证据否定的假设

| 假设 | 证据 | 判定 |
|---|---|---|
| **AMSS 内部断言 (assert / ERR_FATAL)** | ① `MDM_ERR_FATAL`（`0x011146d8`，4224 B）**全 0**；② `MDM2AP_ERRFATAL` GPIO 全程 **lo**；③ 内核没有收到 `ESOC_ERR_FATAL` 事件；④ `esoc-mdm-4x.c` 的 ERR_FATAL 路径会置 `AP2MDM_ERRFATAL`+`AP2MDM_VDDMIN`，实测都没有。 | **否定（强）** |
| **kickstart / 我们的 loader 复位了 modem** | `ks.real` 正常启动路径无任何复位动作（§D.2/§D.4）；v39 的自伤代码已移除且 11.2 s 依旧。 | **否定（强）** |
| **AP 侧主动 assert reset** | 简报：`AP2MDM_SOFT_RESET` 始终 hi；`mdm_status_change()` 是 `value==0 && mdm->ready` 才报 `unexpected reset`，即**是 modem 自己把 STATUS 拉低**。 | **否定（强）** |

### E.2 最强假设：modem 侧看门狗 bite / 不经 err_fatal 的内部复位（**推测，中等偏强**）

**判断依据：**

1. **"有复位记录、没有故障记录"这个组合是看门狗复位的教科书特征。**
   `RST_STAT` 区非 0（`=2`）说明复位被引导链**观察到并归类**了；而 `mdm_err` 全 0 说明**软件没有机会（或没有走）致命错误路径**。如果是 AMSS 主动 assert，它会先写 `mdm_err` + 拉 `MDM2AP_ERRFATAL` —— 两件事都没发生。
2. **11.2 秒极其稳定（11.19 / 11.18 / 11.20）**，方差 < 0.02 s。这种"教科书式精确"的间隔是**硬件/看门狗计数器**的典型特征，而不是软件竞态或外部事件（外部事件会有抖动）。
3. **固件里确实有完整的看门狗设施**（§C.4）：`dog` 任务、`dog_hb:ping` 心跳、`Dog bark timeout.`、`Watchdog startup timeout`、`Watchdog detects task starvation %d`、`Dog Report Information (dog_state_table)`、`sys_m_wdog_reset_isr`，以及 MDM9x35 上唯二能整芯片复位的 `MPM2_WDOG` / `MPM2_PSHOLD`。
4. **`SFR Init: wdog or kernel error suspected.`（`qdsp6sw.mbn` @ `0x1bd6dd0`）** 直接证明：**这个平台在下次启动时会去读"上一次是不是看门狗复位"**。也就是说"看门狗复位"在这个平台上是**一个被显式处理的复位类别**。
5. 复位后 `MDM2AP_STATUS` 被拉低（而不是 errfatal 被拉高），与"芯片直接复位"一致。

**这条假设的弱点**：我没有把 `RST_STAT = 2` 与"WDOG"在二进制层面钉死（§A.4 已说明固件里没有这个枚举）。**所以它是"最符合所有观测"的假设，不是确证。**

### E.3 次强假设：AMSS 等 AP 侧启动握手超时后自复位（**推测，中等**）

**判断依据：**

1. **原厂 AP 侧流程（`stock/qcom-bin/mdm_helper`，27272 B，md5 `6d2bbae9…`）在"镜像传完"之后还有 4 个动作**，而当前流程一个都没做：

```
$ strings -a -n 3 stock/qcom-bin/mdm_helper | grep -E "MDM2AP|efs|ESOC|hsic|HSIC|diag|IMG_"
%s: Setting up mdm helper device structure
%s: Found private data for MDM9x35            ESOC Details: Name:MDM9x35 Port:/dev/esoc-0 Link:HSIC
%s: %s: Initiating HSIC bind                  -> /sys/bus/platform/drivers/s5p-ehci/bind  (15510000.usb)
%s: Sending boot status notification to HSIC
%s: %s:hsic_ready failed
/dev/ks_hsic_bridge
/dev/efs_hsic_bridge                          <-- ★ 简报说"始终不出现"
%s: Failed to send IMG_XFER_DONE notification
%s: MDM2AP_STATUS is now high
%s: Waiting for mdm boot                      <-- 等 modem 起来
MDM did not set MDM2AP_STATUS high
%s: Setting up %s link for efs_sync           <-- ★ 起来之后再建一条 EFS sync 链路
%s: Could not detect EFS sync port
%s: No efs_sync_device specified for target
%s: Failed to create efs sync process
%s: Failed to exec KS process for efs sync    <-- ★ 再 exec 一次 /system/bin/ks (-l ...)
/dev/diag
%s: Failed to send ESOC_DIAG_DISABLE notification
%s: Failed to send ESOC_BOOT_DONE notification <-- ★ 最后才发 BOOT_DONE
```

   即原厂流程是：**传镜像 → HSIC bind → 等 `MDM2AP_STATUS` 高 → 建 EFS-sync 链路并跑第二遍 ks → 发 `ESOC_BOOT_DONE` → 发 `ESOC_DIAG_DISABLE`**。
   当前流程（`powerup` + `esoc_hsic` + `ldrx`）**只做了"传镜像"**，后 4 步全缺，`/dev/efs_hsic_bridge` 也确实从未出现。

2. **内核侧印证 `ESOC_BOOT_DONE` 是"启动完成"的信号**（`drivers/esoc/esoc-mdm-4x.c:563`）：

```c
case ESOC_BOOT_DONE:
        esoc_clink_evt_notify(ESOC_RUN_STATE, esoc);      // -> mdm_drv->mode = RUN
        break;
```
   而 `esoc-mdm-drv.c:65` 的 `ESOC_RUN_STATE` 正是 `complete(&boot_done)`（"mdm is now ready"）的来源。**也就是说这条 `BOOT_DONE` 通知是原厂启动流程的收尾动作，当前流程从未发出。**

3. **AMSS 侧确有大量"等 host/AP"的超时**（§C.3）：`mhi_core_link_completion_timer_cb: Timeout`、`HSU LPM HSIC timed out.`、`Timeout occured waiting for data`、`remotefs_sio_flush_tx: Timeout during sio_flush_tx`、`Diag timed out on SIO callback`、`sys_m: SSR notification ACK fail`。**其中 `remotefs_sio_flush_tx` 就直接对应 EFS/remote-fs 同步链路——正是当前缺失的那条。**

**这条假设的弱点**：
* AMSS 里 `- Ignore the efs_sync!`（`apps.mbn` @ `0x166104` / `0x26a4db`）暗示 EFS sync **可以被忽略**，不一定是致命的；
* 我**没有找到任何能与 11.2 s 对上号的超时常量**；
* 如果 AMSS 是"有秩序地"超时重启，它更可能走 `system:reset` 路径，而这类复位通常也会留痕（`cp_crash_history.txt`、SFR）——但 `mdm_err` 是空的。

### E.4 最终裁决

> **11.2 秒复位最可能是 modem 侧的看门狗（dog bite）或一条不经过 err_fatal 的内部/硬件复位**（**推测，中等偏强**）；
> **"等 AP 完成启动握手（HSIC/MHI/EFS-sync/ESOC_BOOT_DONE）超时"是并列需要排查的第二假设**（**推测，中等**）；
> **AMSS 断言 / ERR_FATAL / 我们的 loader / AP 主动复位 —— 均已排除（强）**；
> **`ResetStatusRegion = 2` 的确切复位源编号 —— 未定**（固件内不存在该枚举，需要 §F 的实验来钉死）。

### E.5 我为什么没能把 A 彻底钉死（方法论限制，如实说明）

1. **`qdsp6sw.mbn` 是 Hexagon（Q6）代码**：56 MB，无符号表，`readelf` 只有 5 个 PT_LOAD；反汇编需要 Hexagon 后端与正确的加载基址映射。我只做了字符串/常量层面的分析，**没有做代码级调用图还原**。
2. **`sbl1.mbn` 写 `RST_STAT` 的那段代码找不到直接字面量**：我在 `sbl1.mbn` 里搜到所有落在 `0x07fb6xxx` 的 32 位立即数（9 处），**没有一处是 `0x07fb64d8`**；最接近的是 `0x07fb64e0`（在描述表里，是数据不是代码）和 `0x07fb64e8`（在 `boot_qfprom_test` 的错误路径里，与复位无关）。写 `0x07fb64d8` 的代码要么用基址+偏移（`0x07f00000` + `0xB64D8`），要么在 **PBL（ROM，我们拿不到）** 里。
3. **没有 reset-reason 枚举可供比对**：这不是搜索不够，而是**素材里不存在**（§A.4 已穷尽证明）。

---

## F. 建议的判决性实验（把 A 从"未定"变成"确证"）

全都是**只读**的采样，不需要改固件：

1. **时序采样 `RST_STAT` / `PMIC_PON`**：在**同一轮**实验里，(a) `powerup` 之后、`ldrx` 之前读一次 `0x07fb64d8`/`0x07fb64e0`；(b) AMSS 起来后读一次；(c) 11.2 s 掉线之后再读一次。三次值的差异能立刻回答"2 描述的是哪一次复位"（§A.5 的不确定性）。
   * 读数方式：modem 起来后用 Sahara `MEMORY_DEBUG` 模式（`ks.real -m` 走的就是这条路，`0x6e3b SAHARA_WAIT_MEMORY_TABLE`）读 4/8 字节即可，不必碰真机固件。
2. **抓 AMSS 侧的崩溃历史**：`cp_crash_history.txt`（NV/EFS 文件，`qdsp6sw.mbn` @ `0x1bd7113` 引用）与 `err_fatal_action`（`/nv/item_files/dnt/err/err_fatal_action`，@ `0x1bd6dfa`）。如果里面有 `wdog`/`bite` 记录，§E.2 直接升级为确证。
3. **补上被省略的 AP 侧收尾动作**：按 `mdm_helper` 的顺序，在镜像传完后接上 `ESOC_BOOT_DONE`（`ESOC_NOTIFY` ioctl，`enum esoc_notify` 值 **2**）与 `/dev/diag` 的 `ESOC_DIAG_DISABLE`（值 **12**）。如果 11.2 s 消失，§E.3 升级为确证。
   * 顺序：`ESOC_IMG_XFER_DONE`(1) → 等 `MDM2AP_STATUS` 高 → EFS sync → `ESOC_BOOT_DONE`(2) → `ESOC_DIAG_DISABLE`(12)。
4. **读 AMSS 的 `dog_state_table`**：`Dog Report Information (dog_state_table)`（`apps.mbn` @ `0x916a0`）会在狗咬之前/之后打印任务表。想办法把 AMSS 的串口/DIAG 日志引出来，就能看到"哪个任务没喂狗"。

---

## G. 我实际用过的命令（可复现）

### G.1 环境

```bash
# capstone 装进独立 venv（不污染系统 python，未装任何重型框架）
python3 -m venv /tmp/csvenv && /tmp/csvenv/bin/pip install --quiet capstone
/tmp/csvenv/bin/python -c "import capstone;print(capstone.__version__)"   # -> 5.0.7

# 可用的反汇编器
which llvm-objdump gdb objdump          # LLVM 23.1.1 / 支持 arm + hexagon target
/home/duanjb666/los20/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin/aarch64-linux-android-objdump
```
我自己写的三个小工具（都在 `/tmp/armdis/`，**没有写进 `baseband-study/`**）：
* `/tmp/armdis/armdis.py <file> <start_va_hex> <end_va_hex> {arm|thumb}` — ARM/Thumb 原始区反汇编，能解析 ELF32 PT_LOAD 与 MBN v3 头（自动算基址）。
* `/tmp/armdis/a64raw.py <file> <lo_hex> <hi_hex>` — AArch64 原始区反汇编（ks.real 的 vaddr == file offset，因为第一个 PT_LOAD 是 offset 0/vaddr 0）。
* `/tmp/armdis/a64s.py <file> <string_va...>` — AArch64 字符串交叉引用（ADRP+ADD / ADRP+LDR，带寄存器失效处理）。

### G.2 描述表 / dump 区（§A.1）

```bash
cd /home/duanjb666/deepseek/G9209-fix/baseband-study
xxd evidence/cpdump/ResetStatusRegion.BIN
xxd evidence/cpdump/PmicPONstat.BIN
cat   evidence/cpdump/CMMScript.BIN
xxd   evidence/cpdump/MDM_ERR_FATAL.BIN | head -80
strings -a -t x -n 4 evidence/cpdump/MDM_ERR_FATAL.BIN        # 空
md5sum evidence/cpdump/*.BIN mdm-crash/mdm_err.*

# 832 字节二进制描述表 -> 16 项 × 52 字节
python3 - <<'PY'
import struct
d=open('evidence/18-mem-dump-832.bin','rb').read()
for k in range(len(d)//0x34):
    o=k*0x34
    en,addr,size=struct.unpack_from('<III',d,o)
    desc=d[o+0x0c:o+0x0c+20].split(b'\x00')[0].decode(errors='replace')
    fn=d[o+0x20:o+0x20+16].split(b'\x00')[0].decode(errors='replace')
    print(f"{k:>2} {en:>2} {addr:#012x} {size:#010x}  {fn:<14} {desc}")
PY

# sbl1 内嵌字符串池
xxd -s 0x104b0 -l 240 stock-cp/image/sbl1.mbn
strings -a -t x -n 3 stock-cp/image/sbl1.mbn | awk 'strtonum("0x"$1)>=0x10200 && strtonum("0x"$1)<=0x10900'
```

### G.3 全镜像字符串与关键词（§A.4 / §C）

```bash
export LC_ALL=C     # ★ 必须；否则 grep -E 的 ASCII 区间在 UTF-8 locale 下会失效
cd /home/duanjb666/deepseek/G9209-fix/baseband-study/stock-cp/image
for f in qdsp6sw apps dsp2 tz rpm mba sdi acdb; do strings -a -t x -n 5 $f.mbn > /tmp/$f.str; done
strings -a -t x -n 5 ../../mdm-crash/DDRCS0.BIN > /tmp/ddrcs0.str

for f in /tmp/*.str; do
  echo "## $f"
  grep -iE "reset.?reason|rst_reason|pon_reason|boot_reason|PON_WARM|PON_HARD|WDOG|watchdog|KPDPWR|PS_HOLD|warm_reset|hard_reset|Reset Reason|RST_STAT" $f | sort -u
done
grep -n -E "9x35|MDM9|9635|AAAHAN" /tmp/qdsp6sw.str /tmp/dsp2.str          # 版本串
awk 'strtonum("0x"$1)>=0x2368f00 && strtonum("0x"$1)<=0x2369500' /tmp/qdsp6sw.str   # MPM2_* 表
awk 'strtonum("0x"$1)>=0x1bd6c00 && strtonum("0x"$1)<=0x1bd7200' /tmp/qdsp6sw.str   # dog / SFR
awk 'strtonum("0x"$1)>=0x254a00 && strtonum("0x"$1)<=0x256400' /tmp/apps.str       # sys_m / err_fatal
```

### G.4 ks.real（§D）

```bash
KS=/home/duanjb666/deepseek/G9209-fix/baseband-study/evidence/ks.real
readelf -h -l -S $KS
strings -a -t x -n 3 $KS                      # 全部字符串（表见 §D.1）

# 还原 struct option[]（通过 .rela.dyn 的 R_AARCH64_RELATIVE addend 找到 name 指针位置）
python3 - <<'PY'
import struct
d=open('/home/duanjb666/deepseek/G9209-fix/baseband-study/evidence/ks.real','rb').read()
e=struct.unpack_from('<Q',d,0x28)[0]; es=struct.unpack_from('<H',d,0x3A)[0]
en=struct.unpack_from('<H',d,0x3C)[0]; sx=struct.unpack_from('<H',d,0x3E)[0]
sh=struct.unpack_from('<Q',d,e+sx*es+0x18)[0]
secs=[]
for i in range(en):
    o=e+i*es; no,t,fl,ad,of,sz=struct.unpack_from('<IIQQQQ',d,o)
    secs.append((d[sh+no:d.find(b'\x00',sh+no)].decode('replace'),ad,of,sz))
names={0x5e42:'help',0x5e47:'port',0x5e4c:'verbose',0x5e54:'command',0x5e5c:'memdump',
       0x5e64:'image',0x5e6a:'sahara',0x5e71:'prefix',0x5e78:'where',0x5e7e:'ramdumpimage',
       0x5e8b:'efssyncloop',0x5e97:'rxtimeout',0x5ea1:'maxwrite',0x5eaa:'addsearchpath'}
for nm,ad,of,sz in secs:
    if nm!='.rela.dyn': continue
    for k in range(sz//24):
        ro,ri,add=struct.unpack_from('<QQq',d,of+k*24)
        if add in names:
            fo=next(s[2]+(ro-s[1]) for s in secs if s[0]=='.data.rel.ro' and s[1]<=ro<s[1]+s[3])
            print(f"entry@{ro:#06x} --{names[add]:<14} has_arg={struct.unpack_from('<i',d,fo+8)[0]} "
                  f"val={struct.unpack_from('<i',d,fo+24)[0]:#x} '-{chr(struct.unpack_from('<i',d,fo+24)[0])}'")
PY

# 关键分支反汇编（vaddr == file offset）
/tmp/csvenv/bin/python /tmp/armdis/a64raw.py $KS 0x3f80 0x4070   # ESOC_SET_CRASH（dump 循环内）
/tmp/csvenv/bin/python /tmp/armdis/a64raw.py $KS 0x4140 0x4210   # SAHARA_WAIT_DONE_RESP 分支
/tmp/csvenv/bin/python /tmp/armdis/a64raw.py $KS 0x5660 0x5740   # "Successfully uploaded all images" -> return 1
/tmp/csvenv/bin/python /tmp/armdis/a64raw.py $KS 0x5cd0 0x5d60   # efssyncloop(-l) 门控循环

# 字符串交叉引用
/tmp/csvenv/bin/python /tmp/armdis/a64s.py $KS 0x732b 0x6ffb 0x6933 0x71c5 0x7237 0x5e8b
```

### G.5 其它（§C.2 / §D.3 / §A.4）

```bash
# USB VID/PID：MOVW 立即数 + 设备描述符字节，双重搜索
python3 - <<'PY'
import glob,struct
def thumb_movw(imm):
    i=(imm>>11)&1; imm4=(imm>>12)&0xf; imm3=(imm>>8)&7; imm8=imm&0xff
    hw1=0xF240|(i<<10)|imm4
    return [struct.pack('<HH',hw1,(imm3<<12)|(rd<<8)|imm8) for rd in range(16)]
def arm_movw(imm):
    imm4=(imm>>12)&0xf; imm12=imm&0xfff
    return [struct.pack('<I',0xE3000000|(imm4<<16)|(rd<<12)|imm12) for rd in range(16)]
for f in sorted(glob.glob('stock-cp/image/*.mbn'))+['mdm-crash/DDRCS0.BIN']:
    d=open(f,'rb').read(); out=[]
    for imm in (0x9048,0x904c,0x9075,0x9008,0x901d):
        for pat in thumb_movw(imm)+arm_movw(imm):
            i=d.find(pat)
            if i>=0: out.append(f"{imm:#06x}@{i:#x}"); break
    if out: print(f, out)
PY
python3 -c "
import glob
for f in sorted(glob.glob('stock-cp/image/*.mbn'))+['mdm-crash/DDRCS0.BIN']:
    d=open(f,'rb').read(); p=bytes.fromhex('12010002'); i=d.find(p)
    while i>=0:
        print(f, hex(i), d[i:i+14].hex(' ')); i=d.find(p,i+1)
"
xxd -s 0x2a120 -l 96 stock-cp/image/sbl1.mbn        # 唯一的 05c6:9008 设备描述符

# DDRCS0 与 apps.mbn 的加载映射
python3 -c "
a=open('stock-cp/image/apps.mbn','rb').read(); b=open('mdm-crash/DDRCS0.BIN','rb').read()
for s in [b'ssr:return:MDM-Q6SW:Wdog bite', b'Set MDM2AP_STATUS_OUT_GPIO high']:
    print(s, hex(a.find(s)), hex(b.find(s)), 'delta', hex(b.find(s)-a.find(s)))
"

# mdm_helper / 内核 ESOC
strings -a -n 3 stock/qcom-bin/mdm_helper
grep -n -E "ESOC_SET_CRASH|ESOC_PANIC|esoc_notify" include/uapi/linux/esoc_ctrl.h   # 在内核源码树里
sed -n '546,600p' drivers/esoc/esoc-mdm-4x.c        # mdm_notify(): ESOC_BOOT_DONE -> ESOC_RUN_STATE
sed -n '56,90p'  drivers/esoc/esoc-mdm-drv.c        # mdm_handle_clink_evt(): ESOC_RUN_STATE -> boot_done
grep -rn "MDM2AP_STATUS_TIMEOUT_MS\|MDM_MODEM_TIMEOUT" drivers/esoc/*.c   # 120000L / 3000
```

---

## H. 附：关键地址 / 偏移速查

| 项 | 地址或偏移 | 值 / 内容 | 来源 |
|---|---|---|---|
| **ResetStatusRegion** | `0x07fb64d8`，4 B | `02 00 00 00` | `18-mem-dump-832.bin` 项[11] + `CMMScript.BIN` + `sbl1.mbn@0x10524` |
| **PmicPONstat** | `0x07fb64e0`，8 B | `20 00 02 00 02 00 00 00` | 同上，项[10] |
| **MDM_ERR_FATAL** | `0x011146d8`，`0x1080` B | **全 0**（三份拷贝 md5 `aa056ba7…`） | 项[13]/[14] |
| CMM Script (load.cmm) | `0x07fa9320`，`0x4bc` B | 文本，Build ID `9x35A-AAAHANAZA-40000000` | 项[15] |
| DDR CS0 | `0x0`，`0x08000000` (128 MB) | 实抓 12 MB | 项[12] |
| SMEM base | `0x07f00000` | 立即数 @ `sbl1.mbn` 文件 `0x2350 / 0x23ac / 0x1c904 / 0x1c908` | sbl1 |
| sbl1 代码基址 | `0xF8004000`（文件 `0x50` 起） | MBN 头 `0x18/0x1c` 字段 | sbl1 |
| sbl1 image_id | `21` | 头 `0x08` | sbl1 |
| apps.mbn 加载点 | 文件 `0x0` ↔ DDR `0x50000`（+0x50000 均匀偏移） | 3 条字符串一致 | apps ↔ DDRCS0 |
| `MDM2AP_STATUS_OUT_GPIO go high` | apps.mbn `0x60dc8` / DDR `0xb0dc8` | 字符串 | apps |
| `sys_m` 任务区 | apps.mbn `0x607e0`–`0x61834` | SSR/SSR-ACK/reset 框架 | apps |
| `System monitor init with high AP2MDM_ERR_FATAL_GPIO` | apps.mbn `0x2554e2` | 字符串 | apps |
| `Dog bark timeout.` / `Watchdog startup timeout` | apps.mbn `0x254ed0` / `0x254f5e` | 字符串 | apps |
| `dog_hb:ping` / `dog_state_table` | qdsp6sw.mbn `0x1bd6d2b` / `0x1bd6d37` | 字符串 | qdsp6sw |
| **`SFR Init: wdog or kernel error suspected.`** | qdsp6sw.mbn **`0x1bd6dd0`**（dsp2.mbn `0x37f6e0`） | 字符串 | qdsp6sw/dsp2 |
| `cp_crash_history.txt` | qdsp6sw.mbn `0x1bd7113` | 字符串 | qdsp6sw |
| `MPM2_WDOG` / `MPM2_PSHOLD` | qdsp6sw.mbn `0x23691af` / `0x23691b9` | HWIO 名表 | qdsp6sw |
| AMSS/MPSS 版本串 | qdsp6sw.mbn `0x1bd86c8` | `MPSS.BO.2.0.1.c3.5-00221-M9635TAARANAZM-1.154625.1.157564.1` | qdsp6sw |
| 唯一的 `05c6:9008` 描述符 | sbl1.mbn `0x2a13c`（idVendor@`0x2a144`） | `12 01 00 02 00 00 00 40 c6 05 08 90` | sbl1 |
| ks.real `struct option[]` | `0x99d8`–`0x9b98` | 14 项，见 §D.1 | ks.real |
| ks.real `ESOC_SET_CRASH` ioctl | 发码点 `0x3fcc`–`0x3ffc`，号 `0x4004cc09` | `_IOW(0xCC,9,u32)` | ks.real + 内核头 |
| ks.real 成功返回点 | `0x5698`–`0x569c` | `mov w0,#1; b 0x5ba0` | ks.real |
| ks.real `-l` 门控 | `0x5d1c` / `0x5d44` | `and w22,w19,#1` / `tbnz w22,#0,0x5d1c` | ks.real |

---

## I. 诚实性声明

* 本报告所有 hexdump、字符串偏移、反汇编片段均来自本机文件，命令见 §G，可复现。
* 标注 **确证** 的条目都有**至少两处独立证据**（或原始字节 + 官方用法文本/内核源码互证）。
* 标注 **推测** 的条目我给出了推理链与**弱点**，请勿当作事实使用。
* 标注 **未定** 的条目（`RST_STAT = 2` 的确切复位源）是**素材本身不足**，不是分析未完成；§F 给出了把它变成确证的实验路径。
* `apps.mbn` / DDRCS0 中存在大量 `Assert failure:` / `panic(...)` **字符串常量**，它们是镜像自带内容，**不代表崩溃发生过**——本报告未把它们当作证据。
