#!/usr/bin/env python3
"""组装 MDM 测试 boot.img —— ★ 安全版（默认沿用已知可启动的 DTB tail）

## 为什么要有这个版本（血泪教训）

旧脚本 `make_mdm_boot.CHN-DTB-BRICK.py` 会把**当前编译出的 5 个 CHN DTB**
（`exynos7420-zeroflte_chn_0{0..4}.dtb`，hw_rev 0..6 / 7 / 8 / 9 / 10..255）
装进 boot.img 的 DTBH tail。

**这套 CHN DTB 在本社区内核上根本起不来** —— 已有三次独立记录：
  - `boot-H-chn-factory.img`、`boot-L-chn-fixed-kernel.img`：各等 5 分钟无 adb
  - `boot-AE-diag.img`（2026-10-03）：刷入后手机**从 USB 总线完全消失**，
    连 Download 模式都没有，最后靠进 TWRP 手工 dd 才恢复

而只要**沿用 `boot-AD-reqleft.img`（已知可启动）的 DTB tail**、仅替换内核，
新内核就能正常启动 —— 已实测验证（`boot-AF-newkern-adtail.img`，内核 #27）。

## 本脚本的行为

- 基座：`boot-AD-reqleft.img`（md5 `5e575b6a0eb83eb5bd1d731349798d9d`）
- 内核：`/home/duanjb666/kbuild/mdm-out/arch/arm64/boot/Image`（当前编译产物）
- DTB tail：**原样复制基座的**（除非显式传 `--rebuild-dtbs`）
- 输出：29,360,128 B（28 MiB），带完整结构回读校验

用法：
    python3 make_boot_safe.py <输出路径> [--rebuild-dtbs]

`--rebuild-dtbs` 是**危险**选项，只有在确认新 DTB 能启动后才用（会打印醒目警告）。
"""
import struct, hashlib, sys, os

BASE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    'boot-AD-reqleft.img')
KERN = '/home/duanjb666/kbuild/mdm-out/arch/arm64/boot/Image'
DTBS = [f'/home/duanjb666/kbuild/mdm-out/arch/arm64/boot/dts/'
        f'exynos7420-zeroflte_chn_{i:02d}.dtb' for i in range(5)]
HW = [(0, 6), (7, 7), (8, 8), (9, 9), (10, 255)]
PART = 29360128


def rd(p):
    return open(p, 'rb').read()


def parts(d):
    ks, ka, rs, ra, ss, sa, tg, page, f40, f44 = struct.unpack_from('<10I', d, 8)
    pg = lambda n: (n + page - 1) // page
    o = page
    kern = d[o:o + ks]; o += pg(ks) * page
    ram = d[o:o + rs];  o += pg(rs) * page
    sec = d[o:o + ss];  o += pg(ss) * page
    return (ks, ka, rs, ra, ss, sa, tg, page, f40, f44), d[:page], kern, ram, sec, d[o:]


def build_tail_from_dtbs(page):
    """危险路径：用当前编译的 CHN DTB 重建 tail。"""
    blobs = [rd(p) for p in DTBS]
    first = 0x800
    offs, cur = [], first
    for b in blobs:
        offs.append(cur)
        cur += (len(b) + page - 1) // page * page
    head = bytearray()
    head += b'DTBH' + struct.pack('<4I', 2, len(blobs), 0x1cfc, 0x50a6)
    for i, b in enumerate(blobs):
        chip, board = (0x1cfc, 0x50a6) if i < len(blobs) - 1 else (0, 0)
        head += struct.pack('<8I', 0x217584da, HW[i][0], HW[i][1],
                            offs[i], len(b), 0x20, chip, board)
    head += b'\x00' * (first - len(head))
    tail = bytes(head)
    for b in blobs:
        tail += b + b'\x00' * ((-len(b)) % page)
    return tail


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    out = args[0] if args else 'boot-safe.img'
    rebuild = '--rebuild-dtbs' in sys.argv

    if not os.path.exists(BASE):
        print(f"!! 基座不存在：{BASE}")
        print("   （它是救命镜像，必须保留）")
        return 1
    base_md5 = hashlib.md5(rd(BASE)).hexdigest()
    if base_md5 != '5e575b6a0eb83eb5bd1d731349798d9d':
        print(f"!! 基座 md5 不是已知可启动的版本：{base_md5}")
        print("   期望 5e575b6a0eb83eb5bd1d731349798d9d —— 请确认后再继续")
        return 1

    hdr, header, okern, oram, osec, otail = parts(rd(BASE))
    kern = rd(KERN)
    page = hdr[7]

    if rebuild:
        print("=" * 68)
        print("★★ 警告：--rebuild-dtbs 会用当前编译的 CHN DTB 重建 tail。")
        print("   CHN DTB 在本社区内核上**起不来**，此选项可能让手机无法开机！")
        print("=" * 68)
        tail = build_tail_from_dtbs(page)
        cnt = struct.unpack_from('<I', tail, 8)[0]
        print(f"   重建 tail：count={cnt}")
    else:
        tail = otail
        print(f"   沿用基座 DTB tail（count={struct.unpack_from('<I', tail, 8)[0]}）")

    buf = bytearray()
    buf += b'ANDROID!'
    buf += struct.pack('<10I', len(kern), hdr[1], len(oram), hdr[3], 0,
                       hdr[5], hdr[6], page, hdr[8], hdr[9])
    buf += header[48:page]
    assert len(buf) == page
    buf += kern + b'\x00' * ((-len(kern)) % page)
    buf += oram + b'\x00' * ((-len(oram)) % page)
    buf += tail
    if len(buf) > PART:
        print(f"!! 组装结果 {len(buf):,} > 分区 {PART:,}，会越界 ✗")
        return 1
    buf += b'\x00' * (PART - len(buf))
    open(out, 'wb').write(bytes(buf))

    md5 = hashlib.md5(bytes(buf)).hexdigest()
    print(f"✅ {out}  {len(buf):,} B  md5={md5}")

    # ---- 结构回读校验 ----
    h2, hd2, k2, r2, s2, t2 = parts(rd(out))
    assert k2 == kern, 'kernel 不一致 ✗'
    assert r2 == oram, 'ramdisk 不一致 ✗'
    assert t2[:4] == b'DTBH', 'tail 不是 DTBH ✗'
    mg, ver, cnt, chip, board = struct.unpack_from('<5I', t2, 0)
    print(f"   DTBH ver={ver} count={cnt} chip=0x{chip:x} board=0x{board:x}")
    allok = True
    for i in range(cnt):
        m2, hw0, hw1, off, sz, c5, c6, c7 = struct.unpack_from('<8I', t2, 0x14 + i * 0x20)
        got = t2[off:off + sz]
        tot = struct.unpack_from('>I', got, 4)[0] if len(got) >= 8 else -1
        ok = (m2 == 0x217584da and got == (rd(BASE)[0:0] or got) and tot == sz)
        # got 内容校验：与基座 tail 中的同一记录比对
        base_tail = otail
        bm2, bhw0, bhw1, boff, bsz, *_ = struct.unpack_from('<8I', base_tail, 0x14 + i * 0x20)
        ok = (m2 == bm2 and hw0 == bhw0 and hw1 == bhw1 and sz == bsz
              and got == base_tail[boff:boff + bsz])
        allok &= ok
        print(f"   rec{i}: hw={hw0}..{hw1} off={off:#x} sz={sz:,} "
              f"dtb_totalsize={tot:,} {'✅' if ok else '✗'}")
    assert allok, 'DTB 记录校验失败 ✗'
    print("✅ 回读校验通过（kernel/ramdisk/DTB 记录全部自洽）")
    print(f"   基座 md5={base_md5}（未被修改）")
    return 0


if __name__ == '__main__':
    sys.exit(main())
