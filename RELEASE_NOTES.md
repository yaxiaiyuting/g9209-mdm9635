# G9209 基带研究 —— 首个发布（可刷包 + 完整文档）

## 这个包是什么
**LineageOS 20（原 fakeman 包，`zeroflte`）+ 本项目内核（`boot-AD`）**，
已针对 **SM-G9209（电信版，外挂 MDM9635）** 打好全部必需补丁，TWRP 直接刷入。

## 为什么需要这个包
原版 LineageOS 20 内核在这台机器上会**卡在第一屏**（PCIe panic + 触摸屏 DTB 问题 + ESOC 相关 panic）。
本包内核包含以下修复：

| 修复 | 作用 |
|---|---|
| PCIe 链路失败不再 panic | 能过第一屏 |
| ESOC/SSR 6 处 panic 抑制 | 系统稳定不重启 |
| modem boot 窗口 60s → 900s | flashless boot 不再被误判失败 |
| 恢复 `RESET_SOC` 复位路径 | 用户态可复位 modem（无需断电）|
| **ks_bridge 写路径聚合转发** | 让原厂 kickstart（ks）能被 modem 正确接受（NAK 5/8 → 0）|

## 包内 boot.img 校验
```
md5  5e575b6a0eb83eb5bd1d731349798d9d
size 29,360,128 bytes
```

## 当前能力与限制（诚实说明）
* ✅ 系统可正常启动与使用（除基带外）
* ✅ 自写 Sahara 加载器可被 modem 完整接受 12 个固件镜像（每个 `status=0`）
* ✅ 可完整 dump modem 内存（14 个区）
* ❌ **仍不能读 SIM / 上网**：modem 收完固件后不进入 AMSS（`MDM2AP_STATUS` 始终为低）；
  且 LOS 20 为国际版 S6 vendor，后续还需移植高通 QMI/RIL 用户态

## 回退
用 TWRP 还原备份，或用 Odin/heimdall 刷回原厂 4 件套（AP/BL/CP/CSC_CTC）。

## 附件（可刷包）校验
```
文件名: g9209-mdm9635-lineage-20.0-20261003-UNOFFICIAL-zeroflte.zip
大小  : 753866568 字节 (718 MB)
md5   : 073e15abb2d9023a01fd025e351e023a
sha256: 2a97713e8ee261d5122aa09e4b52f9b350c6d91b63a89fec2978d720959fa500
包内 boot.img md5: 5e575b6a0eb83eb5bd1d731349798d9d
```

## 刷机
1. 下载下面的 zip
2. 进 TWRP → Install → 选择该 zip → 滑动刷入
3. （务必先备份；回退用 Odin/heimdall 刷原厂 4 件套）
