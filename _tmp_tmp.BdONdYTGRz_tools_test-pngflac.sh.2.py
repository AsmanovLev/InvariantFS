import numpy as np, wave, struct
D = "/dev/shm/w12cpngflac/orig"
sr = 44100
t = np.arange(sr, dtype=np.float64) / sr
l = (np.sin(2 * np.pi * 440 * t) * 0.6 * 32767).astype(np.int16)
r = (np.sin(2 * np.pi * 554 * t) * 0.5 * 32767).astype(np.int16)
w = wave.open(D + "/tone16.wav", "wb")
w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr)
w.writeframes(b"".join(struct.pack("<hh", int(a), int(b))
                       for a, b in zip(l, r)))
w.close()
sr2 = 22050
t2 = np.arange(sr2 // 2, dtype=np.float64) / sr2
v = (np.sin(2 * np.pi * 330 * t2) * 0.4 * 8388607).astype(np.int32)
data = bytearray()
for x in v:
    data += int(x).to_bytes(4, "little", signed=True)[:3]
w = wave.open(D + "/tone24.wav", "wb")
w.setnchannels(1); w.setsampwidth(3); w.setframerate(sr2)
w.writeframes(bytes(data)); w.close()
from PIL import Image
Image.fromarray((np.random.default_rng(3).integers(0, 255, (48, 48, 3)))
                .astype("uint8")).save(D + "/cover.png")
