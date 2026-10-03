#!/usr/bin/env python3
"""simg2raw.py — Android sparse image -> raw image（纯 Python，无依赖）

Android sparse 格式（little-endian）：
  file header (32B):
    u32 magic = 0xed26ff3a
    u16 major, u16 minor
    u16 file_hdr_sz (=32)
    u16 chunk_hdr_sz (=16)
    u32 blk_sz
    u32 total_blks       (raw 镜像总块数)
    u32 total_chunks
    u32 image_checksum
  chunk header (16B):
    u16 chunk_type (0xCAC1 raw / 0xCAC2 fill / 0xCAC3 don't care / 0xCAC4 crc32)
    u16 reserved
    u32 chunk_sz         (以 blk_sz 为单位的块数)
    u32 total_sz         (该 chunk 在文件里占的字节数，含 16B 头)

用法: simg2raw.py <input.img> <output.img>
"""
import struct, sys, os

SPARSE_MAGIC = 0xED26FF3A
CHUNK_RAW = 0xCAC1
CHUNK_FILL = 0xCAC2
CHUNK_DONT_CARE = 0xCAC3
CHUNK_CRC32 = 0xCAC4


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]

    with open(src, 'rb') as fi:
        hdr = fi.read(32)
        if len(hdr) < 32:
            print("文件太小"); return 1
        # 布局: u32 magic | u16 major | u16 minor | u16 file_hdr_sz | u16 chunk_hdr_sz
        #       | u32 blk_sz | u32 total_blks | u32 total_chunks | u32 checksum
        (magic, major, minor, file_hdr_sz, chunk_hdr_sz,
         blk_sz, total_blks, total_chunks, image_checksum) = \
            struct.unpack('<IHHHHIIII', hdr[:28])   # py3.14 需精确切片

        if magic != SPARSE_MAGIC:
            print(f"不是 Android sparse 镜像（magic=0x{magic:08x}）")
            return 1
        if file_hdr_sz > 32:
            fi.read(file_hdr_sz - 32)

        raw_size = total_blks * blk_sz
        print(f"sparse: blk_sz={blk_sz} total_blks={total_blks} "
              f"chunks={total_chunks}")
        print(f"raw 大小: {raw_size:,} 字节 ({raw_size/1073741824:.2f} GB)")
        print(f"输出: {dst}")

        written = 0
        # 预分配，避免碎片
        with open(dst, 'wb') as fo:
            try:
                os.posix_fallocate(fo.fileno(), 0, raw_size)
            except Exception:
                fo.truncate(raw_size)
                fo.seek(0)

            zero_blk = b'\x00' * blk_sz
            n_raw = n_fill = n_dc = n_crc = 0

            for i in range(total_chunks):
                ch = fi.read(chunk_hdr_sz)
                if len(ch) < chunk_hdr_sz:
                    print(f"!! chunk {i} 头被截断"); return 1
                ctype, _res, csz, tsz = struct.unpack('<2H2I', ch[:12])   # py3.14 需精确切片
                payload = tsz - chunk_hdr_sz

                if ctype == CHUNK_RAW:
                    left = csz * blk_sz
                    while left > 0:
                        buf = fi.read(min(left, 1 << 22))
                        if not buf:
                            print("!! raw chunk 数据不足"); return 1
                        fo.write(buf); left -= len(buf); written += len(buf)
                    n_raw += 1
                elif ctype == CHUNK_FILL:
                    fill = fi.read(4)
                    if len(fill) < 4:
                        print("!! fill chunk 缺数据"); return 1
                    left = csz * blk_sz
                    blk = fill * (blk_sz // 4)
                    while left > 0:
                        n = min(left, len(blk))
                        fo.write(blk[:n]); left -= n; written += n
                    n_fill += 1
                elif ctype == CHUNK_DONT_CARE:
                    left = csz * blk_sz
                    while left > 0:
                        n = min(left, blk_sz)
                        fo.write(zero_blk[:n]); left -= n; written += n
                    n_dc += 1
                elif ctype == CHUNK_CRC32:
                    fi.read(payload) if payload > 0 else None
                    n_crc += 1
                else:
                    print(f"!! 未知 chunk 类型 0x{ctype:04x} (chunk {i})"); return 1

                if (i + 1) % 500 == 0:
                    print(f"  ... {i+1}/{total_chunks} chunks, "
                          f"{written/1073741824:.2f} GB")

        print(f"完成: raw={written:,} 字节  期望={raw_size:,}")
        print(f"chunk 统计: raw={n_raw} fill={n_fill} dontcare={n_dc} crc32={n_crc}")
        if written != raw_size:
            print("!! 大小不符"); return 1
        print("✅ OK")
    return 0


if __name__ == '__main__':
    sys.exit(main())
