# Winamp 2.95 インストーラ（NSIS, 非 solid, deflate）からデータブロックを全部取り出す。
# ファイル名はヘッダを解析せず、PE はエクスポート名・バージョン情報から後で付ける。
import sys, struct, zlib, os
src, outdir = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
off = next(o for o in range(0, len(d), 512)
           if d[o+4:o+20] == struct.pack('<I', 0xDEADBEEF) + b'NullsoftInst')
flags, sig, m, hlen, alen = struct.unpack('<II12sII', d[off:off+28])
end = off + alen
p = off + 28
os.makedirs(outdir, exist_ok=True)
i = 0
while p + 4 <= end - 4:
    n, = struct.unpack('<I', d[p:p+4]); p += 4
    comp, n = n & 0x80000000, n & 0x7fffffff
    blk = d[p:p+n]; p += n
    try:
        out = zlib.decompressobj(-15).decompress(blk) if comp else blk
    except zlib.error as e:
        print("ERR", i, hex(n), comp, e); out = b""
    name = 'header.bin' if i == 0 else 'blk%03d_%x.bin' % (i, p - n - 4 - off - 28)
    open(os.path.join(outdir, name), 'wb').write(out)
    print(name, len(out), out[:2])
    i += 1
