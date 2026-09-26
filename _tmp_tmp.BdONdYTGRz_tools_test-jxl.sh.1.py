import os
import numpy as np
from PIL import Image

rng = np.random.default_rng(11)
d = "/dev/shm/wp11jxl/orig"

def photo(name, w, h, sigma, quality=85):
    x = np.linspace(0, 255, w, dtype=np.float32)
    y = np.linspace(0, 255, h, dtype=np.float32)[:, None]
    n = rng.normal(0, sigma, (h, w))
    r = np.clip(x + n, 0, 255)
    g = np.clip(y + n, 0, 255)
    b = np.clip((x + y) / 2 + n, 0, 255)
    im = Image.fromarray(np.dstack([r, g, b]).astype(np.uint8))
    im.save(os.path.join(d, name), "JPEG", quality=quality)

# ~50KB..2MB at q85, rising size and noise
photo("p1.jpg",  480,  360, 14)
photo("p2.jpg",  640,  480, 18)
photo("p3.jpg", 1024,  768, 16)
photo("p4.jpg", 1400, 1050, 14)
photo("p5.jpg", 1920, 1280, 12)
# tiny but still a valid baseline JPEG
photo("tiny.jpg", 128, 128, 20)
for f in sorted(os.listdir(d)):
    print(" ", f, os.path.getsize(os.path.join(d, f)), "bytes")
