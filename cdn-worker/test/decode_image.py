"""Decode one image read from stdin with Pillow and print facts about it as JSON.

Used by worker.test.mjs as a second, independent decoder (Pillow's PNG reader
checks chunk CRCs in verify(); its JPEG reader is libjpeg).
"""
import io
import json
import sys

from PIL import Image

data = sys.stdin.buffer.read()
with Image.open(io.BytesIO(data)) as im:
    im.verify()
with Image.open(io.BytesIO(data)) as im:
    im.load()
    print(json.dumps({
        'format': im.format,
        'mode': im.mode,
        'size': list(im.size),
        'extrema': im.getextrema(),
        'progressive': bool(im.info.get('progressive') or im.info.get('progression')),
    }))
