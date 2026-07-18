"""
PNG → ICO 转换脚本
将 res/icon.png 转为包含多尺寸的 res/icon.ico
供 Windows EXE 资源引用（APP_ICON_RC）
"""
from PIL import Image
from pathlib import Path

src = Path(__file__).parent / "icon.png"
dst = Path(__file__).parent / "icon.ico"

# Windows 图标标准尺寸（多尺寸嵌入，系统按场景自动选择）
sizes = [16, 24, 32, 48, 64, 128, 256]

print(f"Converting {src} -> {dst}")
img = Image.open(src).convert("RGBA")
print(f"Source size: {img.size}")

# 保存为多尺寸 ICO
img.save(dst, format="ICO", sizes=[(s, s) for s in sizes])
print(f"OK: {dst} ({dst.stat().st_size} bytes), embedded sizes: {sizes}")
