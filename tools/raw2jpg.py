#!/usr/bin/env -S uv run --quiet --script
# SPDX-License-Identifier: GPL-2.0
# /// script
# requires-python = ">=3.11"
# dependencies = ["numpy", "pillow"]
# ///
# Convert one surface_ultra_cam frame (1920x1080 MTK-packed GBRG10, stride 2400) to JPG.
#   raw2jpg.py frame.raw out.jpg
import sys, numpy as np
from PIL import Image
W, H, S, BL = 1920, 1080, 2400, 64
d = np.frombuffer(open(sys.argv[1], 'rb').read(), np.uint8)[:S * H].reshape(H, S, 1)
d = d.reshape(H, W // 4, 5).astype(np.int64)
v = d[:, :, 0] | (d[:, :, 1] << 8) | (d[:, :, 2] << 16) | (d[:, :, 3] << 24) | (d[:, :, 4] << 32)
img = np.stack([(v >> (10 * k)) & 0x3ff for k in range(4)], -1).reshape(H, W)
img = np.clip(img - BL, 0, None).astype(float)
G1, B, R, G2 = img[0::2, 0::2], img[0::2, 1::2], img[1::2, 0::2], img[1::2, 1::2]   # GBRG
rgb = np.stack([R, (G1 + G2) / 2, B], -1)
rgb *= rgb[..., 1].mean() / rgb.reshape(-1, 3).mean(0)          # gray-world white balance
rgb = np.clip(rgb / np.percentile(rgb, 99), 0, 1) ** (1 / 2.2)
Image.fromarray((rgb * 255).astype(np.uint8)).save(sys.argv[2], quality=92)
print(sys.argv[2], "mean raw", round(float(img.mean()), 1))
