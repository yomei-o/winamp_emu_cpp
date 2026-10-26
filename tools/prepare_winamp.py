# Winamp 2.95 のインストーラ（orig/winamp295.exe、NSIS）から、エミュレータで動かす
# ファイルを取り出して <出力先>/Winamp/ に並べる。
#
#   python tools/prepare_winamp.py orig/winamp295.exe build/drive_c
#
# NSIS のヘッダ（ファイル名の表）は読まず、データブロックを全部展開してから
# PE のエクスポート名かバージョン情報の OriginalFilename で名前を決める。
import os
import struct
import sys
import zlib

# 取り出すもの（小文字の名前 -> Winamp フォルダからの相対パス）
WANTED = {
    'winamp.exe': 'winamp.exe',
    'in_mp3.dll': 'Plugins/in_mp3.dll',
    'out_wave.dll': 'Plugins/out_wave.dll',
}

# 初回起動のダイアログ（ユーザー登録・更新確認）とミニブラウザを出さない設定
WINAMP_INI = (
    '[Winamp]\r\n'
    'newverchk=0\r\n'
    'newverchk2=0\r\n'
    'inet_mode=2\r\n'
    'mb_open=0\r\n'
    'check_ft_startup=0\r\n'
    'outname=out_wave.dll\r\n'
    'splash=0\r\n'
)


def blocks(data):
    off = next(o for o in range(0, len(data), 512)
               if data[o + 4:o + 20] == struct.pack('<I', 0xDEADBEEF) + b'NullsoftInst')
    _flags, _sig, _magic, _hlen, alen = struct.unpack('<II12sII', data[off:off + 28])
    p, end = off + 28, off + alen
    while p + 4 <= end - 4:
        n, = struct.unpack('<I', data[p:p + 4])
        p += 4
        comp, n = n & 0x80000000, n & 0x7FFFFFFF
        raw = data[p:p + n]
        p += n
        try:
            yield zlib.decompressobj(-15).decompress(raw) if comp else raw
        except zlib.error:
            pass  # 使わないブロック（解凍できないものが一つある）


def pe_name(b):
    """エクスポート名、なければ VS_VERSIONINFO の OriginalFilename。"""
    if b[:2] != b'MZ' or len(b) < 0x200:
        return None
    pe = struct.unpack_from('<I', b, 0x3C)[0]
    if b[pe:pe + 4] != b'PE\0\0':
        return None
    nsec = struct.unpack_from('<H', b, pe + 6)[0]
    optsz = struct.unpack_from('<H', b, pe + 20)[0]
    opt = pe + 24
    secs = [struct.unpack_from('<8sIIII', b, opt + optsz + i * 40) for i in range(nsec)]

    def rva2off(rva):
        for _name, vsize, va, rawsize, rawptr in secs:
            if va <= rva < va + max(vsize, rawsize):
                return rva - va + rawptr
        return None

    exp_rva = struct.unpack_from('<I', b, opt + 96)[0]
    if exp_rva:
        o = rva2off(exp_rva)
        if o is not None:
            name_off = rva2off(struct.unpack_from('<I', b, o + 12)[0])
            if name_off is not None:
                return b[name_off:b.index(b'\0', name_off)].decode('latin-1').lower()
    key = 'OriginalFilename'.encode('utf-16-le') + b'\0\0'
    i = b.find(key)
    if i >= 0:
        j = i + len(key)
        while b[j:j + 2] == b'\0\0':
            j += 2
        k = j
        while b[k:k + 2] != b'\0\0':
            k += 2
        return b[j:k].decode('utf-16-le').lower()
    return None


def main():
    src, out = sys.argv[1], sys.argv[2]
    data = open(src, 'rb').read()
    root = os.path.join(out, 'Winamp')
    found = set()
    for b in blocks(data):
        name = pe_name(b)
        if name in WANTED and name not in found:
            dst = os.path.join(root, WANTED[name])
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, 'wb').write(b)
            found.add(name)
            print(WANTED[name], len(b))
    missing = set(WANTED) - found
    if missing:
        sys.exit('not found in the installer: ' + ', '.join(sorted(missing)))
    ini = os.path.join(root, 'winamp.ini')
    if not os.path.exists(ini):
        open(ini, 'w', newline='').write(WINAMP_INI)


main()
