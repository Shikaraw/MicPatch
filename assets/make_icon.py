from pathlib import Path

from PIL import Image, ImageDraw


ROOT = Path(__file__).resolve().parent
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def icon(size):
    scale = size * 4 / 256
    side = size * 4
    image = Image.new('RGBA', (side, side))
    draw = ImageDraw.Draw(image)

    def box(*coords):
        return tuple(round(value * scale) for value in coords)

    def rounded(coords, radius, fill):
        draw.rounded_rectangle(box(*coords), radius=max(1, round(radius * scale)), fill=fill)

    def line(points, weight, color):
        pts = [box(*point) for point in points]
        draw.line(pts, fill=color, width=max(1, round(weight * scale)), joint='curve')
        r = weight / 2
        for x, y in (points[0], points[-1]):
            draw.ellipse(box(x-r, y-r, x+r, y+r), fill=color)

    rounded((8, 8, 248, 248), 56, '#486462')
    gradient = Image.new('RGBA', image.size)
    brush = ImageDraw.Draw(gradient)
    for y in range(side):
        t = y / (side - 1)
        brush.line((0, y, side, y), fill=(round(40-25*t), round(58-35*t), round(58-34*t), 255))
    mask = Image.new('L', image.size)
    ImageDraw.Draw(mask).rounded_rectangle(box(12, 12, 244, 244), radius=round(52*scale), fill=255)
    image.paste(gradient, mask=mask)

    mint = '#79E8C8'
    signal = '#5EC8AD'
    line(((42, 117), (42, 143)), 12, signal)
    line(((214, 117), (214, 143)), 12, signal)
    if size >= 32:
        line(((61, 98), (61, 160)), 8, '#408B7A')
        line(((195, 98), (195, 160)), 8, '#408B7A')

    rounded((100, 47, 156, 146), 28, mint)
    if size >= 24:
        line(((116, 75), (140, 75)), 6, '#216455')
        line(((116, 89), (140, 89)), 6, '#216455')
    else:
        line(((115, 78), (141, 78)), 10, '#216455')

    draw.arc(box(79, 77, 177, 180), start=0, end=180, fill=mint, width=max(1, round(11 * scale)))
    line(((85, 115), (85, 129)), 11, mint)
    line(((171, 115), (171, 129)), 11, mint)
    line(((128, 174), (128, 199)), 11, mint)
    line(((107, 201), (149, 201)), 11, mint)
    return image.resize((size, size), Image.Resampling.LANCZOS)


if __name__ == '__main__':
    frames = [icon(size) for size in SIZES]
    frames[-1].save(ROOT / 'micpatch.png', optimize=True)
    frames[-1].save(ROOT / 'micpatch.ico', sizes=[(s, s) for s in SIZES], append_images=frames[:-1])
