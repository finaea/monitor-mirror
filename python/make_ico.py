"""Generate mirror.ico (multi-size) + a preview PNG. Same design as the tray icon:
a wide monitor with the right half lit cyan = the mirrored region."""
from PIL import Image, ImageDraw

S = 256
img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
d = ImageDraw.Draw(img)
frame = (32, 40, 51, 255)
dim   = (20, 50, 58, 255)
lit   = (53, 214, 240, 255)

# bezel (wide screen)
bx0, by0, bx1, by1 = 20, 72, 236, 176
d.rounded_rectangle([bx0, by0, bx1, by1], radius=14, fill=frame)
# inner screen: left half dim (source), right half lit (mirrored)
ix0, iy0, ix1, iy1 = 34, 86, 222, 162
mid = (ix0 + ix1) // 2
d.rectangle([ix0, iy0, mid, iy1], fill=dim)
d.rectangle([mid, iy0, ix1, iy1], fill=lit)
d.line([mid, iy0, mid, iy1], fill=frame, width=2)
# stand
d.rectangle([S // 2 - 10, by1, S // 2 + 10, by1 + 22], fill=frame)
d.rounded_rectangle([S // 2 - 44, by1 + 22, S // 2 + 44, by1 + 34], radius=5, fill=frame)

img.save("mirror.ico", sizes=[(256, 256), (128, 128), (64, 64), (48, 48), (32, 32), (16, 16)])
img.save("icon_preview.png")
print("wrote mirror.ico + icon_preview.png")
