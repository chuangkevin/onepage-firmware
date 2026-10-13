#!/usr/bin/env python3
"""Generate a 480x800 1-bit badge BMP for the OnePage sleep screen.

This exists because the on-device badge page cannot be relied on: the firmware's
nameplate render path intermittently wedges this panel, while the plain
"/sleep.bmp on the card" path is the one that has always worked. So render the
badge here, offline, and drop the result on the card.

    python3 scripts/make_badge.py --name "KEVIN CHUANG" --title "BACKEND ENGINEER" \
        --id SR-9508 --division "INFORMATION TECHNOLOGY" \
        --code https://interagent.io --out badge.bmp

Then copy it to the card root as /sleep.bmp and set
Settings -> Display -> Sleep screen -> Custom (自訂).

Coordinates are the ones measured off the reference badge (the community one),
so the layout matches it; the sara mark is that reference's own mark, extracted
from its BMP into scripts/assets/sara-mark.png.

Requires: Pillow, and qrcode for --code-type qr.
"""

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

W, H = 480, 800
HERE = Path(__file__).resolve().parent
MARK = HERE / "assets" / "sara-mark.png"

# Same faces the reference uses, via macOS system copies.
F_NAME = "/System/Library/Fonts/Supplemental/DIN Condensed Bold.ttf"
F_TITLE = "/System/Library/Fonts/Supplemental/DIN Alternate Bold.ttf"
F_SMALL = "/System/Library/Fonts/Menlo.ttc"
F_SMALL_FALLBACKS = [
    "/System/Library/Fonts/Supplemental/Andale Mono.ttf",
    "/System/Library/Fonts/HelveticaNeue.ttc",
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
]

M = 28               # content margin, as the reference uses
RW = W - 2 * M


def font(path, size, index=0):
    return ImageFont.truetype(path, size, index=index)


def small_font(size, bold=False):
    for p in [F_SMALL] + F_SMALL_FALLBACKS:
        try:
            return font(p, size, 1 if (bold and p.endswith(".ttc")) else 0)
        except Exception:
            continue
    raise SystemExit("no usable small font found")


def tracked(draw, text, x, baseline, f, spacing, fill, center=False):
    """Draw text with extra letter spacing. `x` is the left edge unless center."""
    widths = [draw.textlength(ch, font=f) for ch in text]
    total = sum(widths) + spacing * (len(text) - 1) if text else 0
    cx = x - total / 2 if center else x
    for ch, w in zip(text, widths):
        draw.text((cx, baseline), ch, font=f, fill=fill, anchor="ls")
        cx += w + spacing
    return total


def fit(draw, text, maxw, maxh, path):
    """Largest size whose rendered text fits maxw x maxh."""
    size = maxh
    while size > 8:
        f = font(path, size)
        bb = draw.textbbox((0, 0), text, font=f)
        if bb[2] - bb[0] <= maxw and bb[3] - bb[1] <= maxh:
            return f
        size -= 1
    return font(path, 8)


def qr_image(payload, size, ecc="M"):
    import qrcode as qr
    q = qr.QRCode(error_correction={"L": qr.constants.ERROR_CORRECT_L,
                                    "M": qr.constants.ERROR_CORRECT_M,
                                    "Q": qr.constants.ERROR_CORRECT_Q,
                                    "H": qr.constants.ERROR_CORRECT_H}[ecc], border=0)
    q.add_data(payload)
    q.make(fit=True)
    m = q.get_matrix()
    n = len(m)
    cell = max(1, size // n)
    img = Image.new("1", (n, n), 1)
    px = img.load()
    for y, row in enumerate(m):
        for x, on in enumerate(row):
            if on:
                px[x, y] = 0
    return img.resize((n * cell, n * cell), Image.NEAREST)


def hatch_footer(draw, foot=16, step=10, width=None):
    width = width or W
    top = (LH if width > W else H) - foot
    H_local = LH if width > W else H
    draw.rectangle([0, top, width - 1, LH - 1 if width > W else H - 1], fill=255)
    for x in range(-foot, width + foot, step * 2):
        draw.polygon([(x, top), (x + step, top),
                      (x + step - foot, top + foot), (x - foot, top + foot)], fill=0)


def render(a):
    img = Image.new("L", (W, H), 255)
    d = ImageDraw.Draw(img)

    # --- black band, y 0..149 ------------------------------------------------
    d.rectangle([0, 0, W - 1, 149], fill=0)
    if a.header:
        f = fit(d, a.header, RW, 64, F_NAME)
        d.text((M, 108), a.header, font=f, fill=255, anchor="ls")
    elif MARK.exists():
        mark = Image.open(MARK).convert("RGBA")
        white = Image.new("L", mark.size, 255)
        img.paste(white, (M, 25), mark)          # mark -> white on the band
    tracked(d, a.subtitle or "STAFF IDENTIFICATION", M, 138, small_font(12, True), 2, 255)

    # --- meta row + rule at y 190 -------------------------------------------
    fs = small_font(14, True)
    if a.employee_id:
        d.text((M, 175), "EMPLOYEE ID  " + a.employee_id, font=fs, fill=0, anchor="ls")
    tw = d.textlength("ACTIVE", font=fs)
    d.text((W - M - tw, 175), "ACTIVE", font=fs, fill=0, anchor="ls")
    d.rectangle([M, 190, W - M - 1, 191], fill=0)

    # --- big name -----------------------------------------------------------
    if a.name:
        f = fit(d, a.name, W - 16, 140, F_NAME)
        d.text((W / 2, 352), a.name, font=f, fill=0, anchor="ms")
    if a.division:
        tracked(d, a.division, W / 2, 393, small_font(12, True), 4.5, 0, center=True)

    # --- title row: rule 443, chip x 324..452 y 456..496, rule 507 ----------
    d.rectangle([M, 443, W - M - 1, 444], fill=0)
    d.text((M, 481), "TITLE", font=small_font(14, True), fill=0, anchor="ls")
    chip = a.title or "-"
    chip_font = small_font(18, True)
    cw = min(RW - 120, max(128, int(d.textlength(chip, font=chip_font)) + 28))
    cx0 = W - M - cw
    d.rectangle([cx0, 456, W - M - 1, 496], fill=0)
    d.text((cx0 + cw / 2, 481), chip, font=chip_font, fill=255, anchor="ms")
    d.rectangle([M, 507, W - M - 1, 508], fill=0)

    # --- code band: bracket box x 129..350 y 523..744 -----------------------
    bx, by, bs = 129, 523, 222
    caption = ""
    if a.code_type == "qr" and a.code:
        q = qr_image(a.code, bs - 22)
        img.paste(q, (bx + (bs - q.width) // 2, by + (bs - q.height) // 2))
        for (x, y, w, h) in [(bx, by, 26, 5), (bx, by, 5, 26),
                             (bx + bs - 26, by, 26, 5), (bx + bs - 5, by, 5, 26),
                             (bx, by + bs - 5, 26, 5), (bx, by + bs - 26, 5, 26),
                             (bx + bs - 26, by + bs - 5, 26, 5), (bx + bs - 5, by + bs - 26, 5, 26)]:
            d.rectangle([x, y, x + w - 1, y + h - 1], fill=0)
        caption = a.code
    elif a.code_type == "barcode" and a.code:
        strip = barcode_image(a.code, RW, 176)
        img.paste(strip, (M, 556))
        # Deliberately no caption: the digits under a 1D symbol are redundant and
        # the user asked for them not to be printed.
        caption = ""

    if caption:
        f = small_font(13, True)
        tw = d.textlength(caption, font=f)
        d.text(((W - tw) / 2, 763), caption, font=f, fill=0, anchor="ls")

    hatch_footer(d)
    return img.point(lambda v: 0 if v < 128 else 255).convert("1")


# Code 39, same table the badge page uses (15-module symbols, one = black).
C39 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-. $/+%*"
C39_PAT = [20957, 29783, 23639, 30485, 20951, 29813, 23669, 20855, 29789, 23645,
           29975, 23831, 30533, 22295, 30149, 24005, 21623, 29981, 23837, 22301,
           30023, 23879, 30545, 22343, 30161, 24017, 21959, 30065, 23921, 22385,
           29015, 18263, 29141, 17879, 29045, 18293, 17783, 29021, 18269, 17477,
           17489, 17681, 20753, 35770]


def barcode_image(value, maxw, height):
    value = value.upper()
    pats = [bin(C39_PAT[C39.index(ch)])[2:] for ch in "*" + value + "*"]
    total = sum(len(p) for p in pats) + (len(pats) - 1) + 20
    mod = max(1, min(4, maxw // total))
    tw = total * mod
    img = Image.new("1", (tw, height), 1)
    px = img.load()
    x = 10 * mod
    for i, pat in enumerate(pats):
        for bit in pat:
            if bit == "1":
                # Fill the whole module, not just its first column: advancing by
                # `mod` after drawing a single column leaves 1px holes and the
                # symbol will not decode.
                for xx in range(x, x + mod):
                    for yy in range(height):
                        px[xx, yy] = 0
            x += mod
        if i != len(pats) - 1:
            x += mod
    return img



# ===========================================================================
# Landscape (800x480 logical). On the device this is selected with
# Settings -> Display -> 方向 = LANDSCAPE_*, which turns the renderer's logical
# coordinates into 800x480, so a bitmap authored at this size maps 1:1.
# ===========================================================================
LW, LH = 800, 480
LFOOT = 16


def landscape_badge(a):
    img = Image.new("L", (LW, LH), 255)
    d = ImageDraw.Draw(img)

    # Black spine on the left carrying the mark, so the wide layout still reads
    # as the same badge rather than a stretched portrait one.
    spine = 250
    d.rectangle([0, 0, spine - 1, LH - LFOOT - 1], fill=0)
    if a.header:
        f = fit(d, a.header, spine - 2 * M, 56, F_NAME)
        d.text((M, 96), a.header, font=f, fill=255, anchor="ls")
    elif MARK.exists():
        mark = Image.open(MARK).convert("RGBA")
        w = spine - 2 * M
        h = max(1, round(mark.height * w / mark.width))
        white = Image.new("L", mark.size, 255)
        img.paste(white.resize((w, h), Image.LANCZOS), (M, 60), mark.resize((w, h), Image.LANCZOS))
    tracked(d, a.subtitle or "STAFF IDENTIFICATION", M, 176, small_font(12, True), 2, 255)

    rx0, rx1 = spine + 30, LW - M
    fs = small_font(14, True)
    if a.employee_id:
        d.text((rx0, 44), "EMPLOYEE ID  " + a.employee_id, font=fs, fill=0, anchor="ls")
    tw = d.textlength("ACTIVE", font=fs)
    d.text((rx1 - tw, 44), "ACTIVE", font=fs, fill=0, anchor="ls")
    d.rectangle([rx0, 66, rx1, 67], fill=0)

    if a.name:
        f = fit(d, a.name, rx1 - rx0, 100, F_NAME)
        d.text(((rx0 + rx1) / 2, 170), a.name, font=f, fill=0, anchor="ms")
    if a.division:
        tracked(d, a.division, (rx0 + rx1) / 2, 204, small_font(12, True), 4.5, 0, center=True)

    d.rectangle([rx0, 232, rx1, 233], fill=0)
    d.text((rx0, 270), "TITLE", font=small_font(14, True), fill=0, anchor="ls")
    chip = a.title or "-"
    cf = small_font(18, True)
    cw = min((rx1 - rx0) - 120, max(128, int(d.textlength(chip, font=cf)) + 28))
    cx0 = rx1 - cw
    d.rectangle([cx0, 246, rx1, 292], fill=0)
    d.text(((cx0 + rx1) / 2, 271), chip, font=cf, fill=255, anchor="ms")
    d.rectangle([rx0, 310, rx1, 311], fill=0)

    if a.code_type == "qr" and a.code:
        q = qr_image(a.code, 140)
        qx = rx1 - 16 - q.width          # keep the brackets off the panel edge
        qy = 300
        img.paste(q, (qx, qy))
        bx, by, bs = qx - 11, qy - 11, 162
        for (x, y, w, h) in [(bx, by, 24, 5), (bx, by, 5, 24),
                             (bx + bs - 24, by, 24, 5), (bx + bs - 5, by, 5, 24),
                             (bx, by + bs - 5, 24, 5), (bx, by + bs - 24, 5, 24),
                             (bx + bs - 24, by + bs - 5, 24, 5), (bx + bs - 5, by + bs - 24, 5, 24)]:
            d.rectangle([x, y, x + w - 1, y + h - 1], fill=0)
        if a.code:
            f = small_font(12, True)
            tw = d.textlength(a.code, font=f)
            d.text((bx - 14 - tw, qy + 70), a.code, font=f, fill=0, anchor="ls")
    elif a.code_type == "barcode" and a.code:
        strip = barcode_image(a.code, rx1 - rx0, 130)
        img.paste(strip, (rx0, 330))

    hatch_footer(d, foot=LFOOT, width=LW)
    return img.point(lambda v: 0 if v < 128 else 255).convert("1")


def landscape_classic(a, dither=2):
    """Photo-led landscape: the picture on the left, the identity on the right."""
    img = Image.new("L", (LW, LH), 255)
    d = ImageDraw.Draw(img)
    px, py, pw, ph = 0, 0, 380, LH - LFOOT

    if a.photo:
        ph_img = Image.open(a.photo).convert("L")
        r = max(pw / ph_img.width, ph / ph_img.height)
        ph_img = ph_img.resize((max(1, round(ph_img.width * r)), max(1, round(ph_img.height * r))), Image.LANCZOS)
        left = (ph_img.width - pw) // 2
        top = (ph_img.height - ph) // 2
        img.paste(ph_img.crop((left, top, left + pw, top + ph)), (px, py))
        img.paste(dither_region(img.crop((px, py, px + pw, py + ph)), dither), (px, py))
    else:
        d.rectangle([px, py, px + pw - 1, py + ph - 1], outline=0, width=2)
        f = small_font(14, True)
        d.text((pw / 2, ph / 2), "NO PHOTO", font=f, fill=0, anchor="mm")

    x0, x1 = 404, LW - M
    fs = small_font(14, True)
    if a.name:
        f = fit(d, a.name, x1 - x0, 78, F_NAME)
        d.text((x0, 96), a.name, font=f, fill=0, anchor="ls")
    if a.title:
        f = fit(d, a.title, x1 - x0, 34, F_TITLE)
        d.text((x0, 142), a.title, font=f, fill=0, anchor="ls")
    if a.division:
        tracked(d, a.division, x0, 176, small_font(12, True), 3, 0)
    if a.employee_id:
        d.text((x0, 208), "EMPLOYEE ID  " + a.employee_id, font=fs, fill=0, anchor="ls")
    d.rectangle([x0, 226, x1, 227], fill=0)

    if a.code_type == "qr" and a.code:
        q = qr_image(a.code, 140)
        img.paste(q, ((x0 + x1) // 2 - q.width // 2, 250))
    elif a.code_type == "barcode" and a.code:
        strip = barcode_image(a.code, x1 - x0, 110)
        img.paste(strip, (x0, 262))

    hatch_footer(d, foot=LFOOT, width=LW)
    return img.point(lambda v: 0 if v < 128 else 255).convert("1")


def dither_region(im, cell):
    """Ordered 4x4 Bayer, each cell covering `cell` x `cell` pixels."""
    bayer = ((0, 8, 2, 10), (12, 4, 14, 6), (3, 11, 1, 9), (15, 7, 13, 5))
    w, h = im.size
    src = im.load()
    out = Image.new("L", (w, h), 255)
    dst = out.load()
    for cy in range(0, h, cell):
        for cx in range(0, w, cell):
            total = n = 0
            for y in range(cy, min(cy + cell, h)):
                for x in range(cx, min(cx + cell, w)):
                    total += src[x, y]
                    n += 1
            v = (total / n - 128) * 1.35 + 138
            v = 0 if v < 0 else (255 if v > 255 else v)
            thr = (bayer[(cy // cell) & 3][(cx // cell) & 3] + 0.5) * 255 / 16
            p = 255 if v > thr else 0
            for y in range(cy, min(cy + cell, h)):
                for x in range(cx, min(cx + cell, w)):
                    dst[x, y] = p
    return out

def main():
    ap = argparse.ArgumentParser(description="Render a 480x800 1-bit badge BMP.")
    ap.add_argument("--name")
    ap.add_argument("--title", default="")
    ap.add_argument("--id", dest="employee_id", default="")
    ap.add_argument("--division", default="")
    ap.add_argument("--subtitle", default="")
    ap.add_argument("--header", default="", help="override the SARA mark with text")
    ap.add_argument("--code", default="")
    ap.add_argument("--code-type", choices=["qr", "barcode", "none"], default="qr")
    ap.add_argument("--orientation", choices=["portrait", "landscape"], default="portrait")
    ap.add_argument("--layout", choices=["badge", "classic"], default="badge",
                    help="landscape only: badge (no photo) or classic (photo-led)")
    ap.add_argument("--photo", default="", help="source image for --layout classic")
    ap.add_argument("--out", default="badge.bmp")
    a = ap.parse_args()
    if not a.name:
        ap.error("--name is required")
    if a.orientation == "landscape":
        img = landscape_classic(a) if a.layout == "classic" else landscape_badge(a)
    else:
        img = render(a)
    img.save(a.out)
    print(f"{a.out}: {img.size[0]}x{img.size[1]} 1-bit ({Path(a.out).stat().st_size} bytes)")


if __name__ == "__main__":
    main()
