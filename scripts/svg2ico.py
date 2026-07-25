"""Convert favicon.svg to app.ico with rounded corners.

Renders the SVG at 256px, applies a rounded-rect mask, then scales down
for all ICO sizes. This gives consistent appearance and soft edges.
"""
import cairosvg
from PIL import Image, ImageDraw
import io

svg_path = 'webui/public/favicon.svg'
ico_path = 'resources/app.ico'
sizes = [256, 128, 64, 48, 32, 16]
corner_radius = 0.15  # 15% of the image size → subtle rounding

# Render SVG at high resolution
png_data = cairosvg.svg2png(url=svg_path, output_width=256, output_height=256)
master = Image.open(io.BytesIO(png_data))
if master.mode != 'RGBA':
    master = master.convert('RGBA')

images = []
for size in sizes:
    img = master.resize((size, size), Image.LANCZOS) if size != 256 else master.copy()

    # Anti-aliased rounded corners: render mask at 4x then downscale
    supersample = size * 4
    r = int(supersample * corner_radius)
    mask_hi = Image.new('L', (supersample, supersample), 0)
    draw = ImageDraw.Draw(mask_hi)
    draw.rounded_rectangle([0, 0, supersample - 1, supersample - 1], radius=r, fill=255)
    mask = mask_hi.resize((size, size), Image.LANCZOS)

    result = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    result.paste(img, (0, 0), mask)
    images.append(result)

images[0].save(ico_path, format='ICO', sizes=[(s, s) for s in sizes], append_images=images[1:])
print(f'Created {ico_path} with sizes: {sizes} (rounded corners)')
