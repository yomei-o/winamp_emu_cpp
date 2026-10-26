# テスト用の MP3 を作る（ドミソドの4音、各1.5秒、44.1kHz ステレオ 128kbps）。
#   pip install lameenc
#   python tools/make_test_mp3.py out.mp3
import sys, math, struct
import lameenc
rate = 44100
notes = [261.63, 329.63, 392.00, 523.25]
pcm = bytearray()
for f in notes:
    n = int(rate * 1.5)
    for i in range(n):
        env = min(1.0, i / 2000, (n - i) / 2000)
        v = int(12000 * env * math.sin(2 * math.pi * f * i / rate))
        pcm += struct.pack('<hh', v, v)
enc = lameenc.Encoder()
enc.set_bit_rate(128)
enc.set_in_sample_rate(rate)
enc.set_channels(2)
enc.set_quality(2)
mp3 = enc.encode(bytes(pcm)) + enc.flush()
open(sys.argv[1], 'wb').write(mp3)
print(sys.argv[1], len(mp3), 'bytes')
