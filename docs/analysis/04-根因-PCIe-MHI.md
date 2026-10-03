# ★★★★★ 根因确认（2026-10-03 14:45）：缺的是 **PCIe + MHI**，不是 USB

## 一、决定性证据：原厂下 modem 的真实形态

在**拿到 root 之后**（Magisk v30.7 补丁成功，`su -c id` → `uid=0`），
终于读到了之前无权访问的关键数据：

### 1.1 modem 的运行期数据通路 = PCIe + MHI + rmnet

```
/sys/bus/pci/devices/0000:01:00.0/
    vendor = 0x17cb        ← Qualcomm
    device = 0x0300        ← MDM9635
    class  = 0xff0000
    driver = mhi           ← ★ MHI over PCIe

属性节点：MHI_M0 / MHI_M1 / MHI_M3 / MHI_MOBILE_HOTSPOT / MHI_STATE

网络接口（原厂正常工作时）：
    rmnet_mhi0: UP, mtu 3584   RX: 72660 bytes / 117 pkts
                                TX: 15728 bytes / 145 pkts   ← ★ 真的在跑！
    rmnet_data0: UP, mtu 1460
    rmnet_data1..5: DOWN（备用）

模块：mhi_sys / mhi_uci / msm_rmnet_mhi / rmnet_data
```

`/sys/bus/pci/drivers/mhi/0000:01:00.0` 存在 → MHI 驱动已绑定该设备。

### 1.2 同时 USB 侧是 8 接口复合设备

```
1-2  05c6:909e  Qualcomm CDMA Technologies MSM
1-2:1.0 → diag_bridge      1-2:1.3 → ks_bridge（创建 /dev/efs_hsic_bridge）
1-2:1.1 → diag_bridge      1-2:1.4/1.5 → mdm_bridge
1-2:1.2 → hsic_sysmon      1-2:1.6 → qc_csvt   1-2:1.7 → ipc_bridge
```

`/dev/efs_hsic_bridge`（`crw------- root root 237,0`）与 `/dev/diag` 均存在。

### 1.3 GPIO（原厂正常）

| GPIO | 值 | 说明 |
|---|---|---|
| `MDM2AP_STATUS` (8) | **hi** | modem 宣告 AMSS 运行中 ✅ |
| `AP2MDM_STATUS` (120) | out **hi** | AP 宣告已上电 ✅ |
| `AP2MDM_HSIC_READY` (135) | out **hi** | HSIC 就绪 ✅ |
| `AP2MDM_SOFT_RESET` (149) | out **hi** | 复位已释放 ✅ |
| `MDM2AP_PBLRDY` (134) | **lo** | 引导已完成（不再处于 PBL）✅ |
| `MDM2AP_ERRFATAL` (24) | lo | 无致命错误 ✅ |

---

## 二、★ 根因：**两条链路都必须建立**

`esoc_link = "HSIC+PCIe"` 这个名字现在完全解释通了：

| 链路 | 用途 | LOS 20 状态 |
|---|---|---|
| **HSIC**（USB） | boot 镜像、EFS 同步、ramdump、diag | ⚠️ 我们做通了（12 镜像全 status=0） |
| **PCIe + MHI + rmnet** | **运行期 QMI 与数据通路** | ❌ **从未枚举出 `0x17cb:0x0300`** |

**modem 启动后需要 PCIe 链路被 AP 枚举成功，才能进入稳定运行态。**
LOS 20 上 PCIe 侧从未出现 Qualcomm 设备 → modem 判定"主机没准备好" →
**约 11.2 秒后自我复位**（与其 crash log「crash happen before running the err_init()」
以及 `MDM_ERR_FATAL` 全零、`ERRFATAL` 全程 lo 完全吻合）。

### 为什么之前所有 HSIC/USB 侧努力都无效
我们把 **HSIC 侧做到了完美**（req engine 常驻、`ESOC_BOOT_DONE`、`HSIC_READY` 四种时机、
EHCI unbind/bind、甚至用原厂 `mdm_helper` 对照），modem 确实起来了（`mdm is now ready`），
**但它要的是 PCIe**。方向从一开始就缺了一半。

---

## 三、下一步（明确）

1. **在 LOS 20 上让 PCIe 枚举出 modem**
   - 检查 `arch/arm64/boot/dts/` 里 `pcie0` 节点与 `qcom,mdm1` 的 `mdm,link` 配置
   - 检查 `drivers/pci/host/pci-exynos.c`（我们为防"卡第一屏"把 panic 改成了打印，
     可能连带把链路建立也跳过了 —— **需要复查**）
   - 对比原厂 `3.10.61` 与我们的 `3.10.108` 在 PCIe 初始化上的差异
2. **`mhi_sys` 模块**：确认它在 LOS 20 上也被正确加载/绑定（原厂有 `mhi_sys`）
3. 一旦 `0000:01:00.0  17cb:0300` 出现且绑上 `mhi` → 再看 `rmnet_mhi0` 是否 UP
4. 然后才轮到 QMI/RIL 用户态

---

## 四、本轮附带成果

- **root 成功**：Magisk v30.7 补丁（主机端重打包，绕过三星 DHTB 的 `magiskboot repack` 失败）
  - `su -c id` → `uid=0(root) context=u:r:magisk:s0`
  - Magisk 版本 `31.0:MAGISKSU`
- **TWRP** 已刷回（从原设备备份 `RECOVERY.img`，md5 `ef70f8e8…`）
- 修掉了 `repack_boot_magisk.py` 的隐患：原来硬编码 xz，
  已改为**按原镜像格式自适应**（原厂 7.0 是 gzip）

---

# 五、★ 为什么 LOS 20 上 PCIe 不枚举 —— 找到我们自己的改动

`drivers/pci/host/pci-exynos.c` 第 379 行附近，**我们**的改动：

```c
if(mdm_get_fatal_status()) {
        return -EPIPE;
} else {
-       panic("PCIE LINK UP FAIL - NEED CP DUMP");
+       /* [MDM-bringup] 原为 panic(...) */
+       dev_err(dev, "%s: MDM PCIe link not up (modem unpowered?) - deferring\n", __func__);
+       return -EPROBE_DEFER;
}
```

## 这段代码说明了什么

1. **原厂这个 `panic()` 就在 `pcie0` 驱动里，而且报的就是 `NEED CP DUMP`**
   —— 这**再次证明 `pcie0@155C0000` 就是接 MDM9635 的 PCIe 控制器**，
   原厂把它当作"modem 链路"来对待，链路不上就 panic。
2. 我们为"防止开机卡第一屏"把它改成了 `-EPROBE_DEFER`（优雅失败）。
   **这是对的方向**（modem 刚开机时确实还没上电），
   **但缺了后半步**：给 modem 上电之后**没有再去触发 PCIe 链路建立 / 重新 probe**。
3. 于是 LOS 20 上 `pcie0` 永远停在 defer 状态 →
   `0000:01:00.0  17cb:0300` 永远不出现 → `mhi` 无设备可绑 → `rmnet_mhi0` 不存在 →
   **modem 等不到运行期数据通路，约 11.2 秒后自复位**。

## 修复方向（明确）

1. **在 modem 上电并完成 Sahara 之后，重新触发 `pcie0` 的链路建立**：
   - 方案 A：内核侧 —— 在 `mdm_status_change()` 检测到 `MDM2AP_STATUS=hi` 后，
     对 `pcie0` 做一次 `pci_rescan_bus()` / 或 unbind+bind 该控制器。
   - 方案 B：用户态 —— 在自写加载器的 runtime handshake 里，
     `HSIC_READY` 之后写 `/sys/bus/pci/rescan`，或 unbind/bind `0000:00:00.0`。
2. **确认 `mhi_sys` 在 LOS 20 上加载并与 `17cb:0300` 绑定**
3. **确认 `rmnet_mhi0` 被创建且 UP**
4. 之后才是 QMI/RIL 用户态（原文档阶段 B/C）

> 这条路径**首次有了明确、可验证的成功判据**：
> `ls /sys/bus/pci/devices/` 里出现 `0000:01:00.0`，且 `driver=mhi`，
> 且 `ip link` 里有 `rmnet_mhi0`。
