import sys, struct
d = open(sys.argv[1], 'rb').read()
for off in range(0, len(d), 512):
    if d[off+4:off+20] == struct.pack('<I', 0xDEADBEEF) + b'NullsoftInst':
        flags, sig, m, hlen, alen = struct.unpack('<II12sII', d[off:off+28])
        print('firstheader at', hex(off), 'flags', flags, 'hlen', hlen, 'alen', alen, 'filelen', len(d))
        print(d[off+28:off+44].hex())
        break
