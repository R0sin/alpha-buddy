"""Check a captured device canvas against its source JPEG (Pillow + numpy).

Usage: python tools/check_jpeg_quality.py .pio/quality-capture
Input: source.jpg, quarter.rgb (160x106 RGB), eighth.rgb (80x53 RGB).
The RGB files must come from M5Canvas.readRectRGB on the same frame.
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw


def main():
    folder = Path(sys.argv[1])
    source = Image.open(folder / "source.jpg").convert("RGB")
    assert source.size == (640, 424), source.size
    quarter = Image.frombytes("RGB", (160, 106), (folder / "quarter.rgb").read_bytes())
    eighth = Image.frombytes("RGB", (80, 53), (folder / "eighth.rgb").read_bytes())
    reference = source.resize((160, 106), Image.Resampling.BOX)
    error = np.abs(np.asarray(quarter, dtype=float) - np.asarray(reference, dtype=float))
    # Detect channel/byte-order mistakes, striped blocks and wrong row stride.
    assert error.mean() < 12, f"quarter decode MAE too high: {error.mean():.2f}"
    assert np.quantile(error.mean(axis=(1, 2)), 0.95) < 16, "bad MCU rows"
    target = source.resize((204, 135), Image.Resampling.BOX)
    old = eighth.resize(target.size, Image.Resampling.NEAREST)
    new = quarter.resize(target.size, Image.Resampling.NEAREST)
    mse = lambda im: np.mean((np.asarray(im, dtype=float) - np.asarray(target, dtype=float)) ** 2)
    assert mse(new) < mse(old), "quarter decode did not retain more source detail"
    preview = Image.new("RGB", (3 * 408, 310), "#eeeeee")
    draw = ImageDraw.Draw(preview)
    for i, (label, im) in enumerate((("Source reference", target),
                                     ("Before: 80 x 53", old),
                                     ("After: 160 x 106", new))):
        draw.text((i * 408 + 12, 12), label, fill="black")
        preview.paste(im.resize((408, 270), Image.Resampling.NEAREST), (i * 408, 36))
    preview.save(folder / "comparison.png")
    print(f"PASS: decode MAE {error.mean():.2f}/255; screen-size MSE "
          f"{mse(old):.2f} -> {mse(new):.2f}")


if __name__ == "__main__":
    main()
