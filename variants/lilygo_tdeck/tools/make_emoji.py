#!/usr/bin/env python3
"""Make the cyberdeck's emoji set: examples/companion_radio/ui-cyberdeck/EmojiData.h

Each emoji is drawn from Noto Emoji (monochrome, SIL Open Font License 1.1,
https://github.com/googlefonts/noto-emoji) into a 16x16 2-bit alpha mask, and given
one tint colour, so the deck can draw it over any background.

  python make_emoji.py path/to/NotoEmoji-Regular.ttf
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

# (emoji, tint RGB, picker name). Order here is the picker's order.
YEL, RED, ORA, GRN, BLU, BRN, GRY, WHT, PNK, PUR, TAN = (
    (255, 204, 51), (240, 60, 60), (255, 140, 30), (90, 200, 90), (80, 160, 255),
    (190, 130, 70), (170, 170, 170), (235, 235, 235), (255, 120, 170), (180, 120, 255), (230, 190, 140))
SET = [
    # reactions first: what people actually send
    ("👍", YEL, "thumbsup"), ("👎", YEL, "thumbsdown"), ("❤", RED, "heart"), ("😂", YEL, "joy"),
    ("🙂", YEL, "smile"), ("😀", YEL, "grin"), ("😉", YEL, "wink"), ("😎", YEL, "cool"),
    ("🤔", YEL, "thinking"), ("😮", YEL, "wow"), ("😢", YEL, "sad"), ("😡", RED, "angry"),
    ("😴", YEL, "sleep"), ("🤣", YEL, "rofl"), ("😅", YEL, "sweat"), ("😊", YEL, "blush"),
    ("😍", YEL, "hearteyes"), ("🙄", YEL, "eyeroll"), ("😬", YEL, "grimace"), ("🥳", YEL, "party"),
    ("👋", YEL, "wave"), ("🙏", YEL, "thanks"), ("👏", YEL, "clap"), ("🤙", YEL, "shaka"),
    ("✌", YEL, "peace"), ("👌", YEL, "ok"), ("💪", YEL, "strong"), ("🤝", YEL, "deal"),
    ("👀", WHT, "eyes"), ("💯", RED, "100"), ("✅", GRN, "check"), ("❌", RED, "x"),
    ("⚠", YEL, "warning"), ("❗", RED, "bang"), ("❓", RED, "question"), ("🆘", RED, "sos"),
    ("🚨", RED, "siren"), ("🔔", YEL, "bell"), ("📢", ORA, "loud"), ("💡", YEL, "idea"),
    # radio & mesh
    ("📡", GRY, "antenna"), ("📶", GRN, "signal"), ("📻", BRN, "radio"), ("📱", GRY, "phone"),
    ("💻", GRY, "laptop"), ("🔋", GRN, "battery"), ("🔌", GRY, "plug"), ("⚡", YEL, "zap"),
    ("🛰", GRY, "satellite"), ("📍", RED, "pin"), ("🗺", TAN, "map"), ("🧭", RED, "compass"),
    ("🌍", BLU, "earth"), ("🌎", BLU, "americas"), ("🌏", BLU, "asia"), ("🔑", YEL, "key"),
    ("🔒", YEL, "lock"), ("⏰", RED, "alarm"), ("⌛", TAN, "hourglass"), ("📞", RED, "call"),
    ("📷", GRY, "camera"), ("🔧", GRY, "wrench"), ("🔨", BRN, "hammer"), ("🛠", GRY, "tools"),
    ("🪓", BRN, "axe"), ("⚙", GRY, "gear"), ("📦", BRN, "box"), ("💰", GRN, "money"),
    ("🎉", PUR, "tada"), ("🎂", PNK, "cake"), ("🏁", WHT, "finish"), ("🚩", RED, "flag"),
    # weather & outdoors
    ("☀", YEL, "sun"), ("🌤", YEL, "sunny"), ("⛅", WHT, "cloudy"), ("☁", WHT, "cloud"),
    ("🌧", BLU, "rain"), ("⛈", GRY, "storm"), ("❄", BLU, "snow"), ("🌨", BLU, "snowing"),
    ("🌫", GRY, "fog"), ("🌪", GRY, "tornado"), ("🌈", PUR, "rainbow"), ("🌙", YEL, "moon"),
    ("⭐", YEL, "star"), ("🔥", ORA, "fire"), ("💧", BLU, "drop"), ("🌊", BLU, "wave"),
    ("🌲", GRN, "evergreen"), ("🌳", GRN, "tree"), ("🌵", GRN, "cactus"), ("🌱", GRN, "sprout"),
    ("🍁", ORA, "leaf"), ("🍂", ORA, "fallen"), ("🌻", YEL, "sunflower"), ("🌽", YEL, "corn"),
    ("🌾", TAN, "grain"), ("🍄", RED, "mushroom"), ("🏔", WHT, "mountain"), ("⛰", BRN, "hill"),
    ("🏕", GRN, "camp"), ("⛺", ORA, "tent"), ("🎣", BLU, "fishing"), ("🌡", RED, "temp"),
    # farm
    ("🏠", TAN, "house"), ("🏡", GRN, "home"), ("🚜", GRN, "tractor"), ("🚗", RED, "car"),
    ("🚙", BLU, "suv"), ("🚚", GRY, "truck"), ("⛽", RED, "fuel"), ("🚲", GRY, "bike"),
    ("🐶", BRN, "dog"), ("🐕", BRN, "dog2"), ("🐱", ORA, "cat"), ("🐔", WHT, "chicken"),
    ("🐓", RED, "rooster"), ("🐣", YEL, "chick"), ("🥚", WHT, "egg"), ("🦆", GRN, "duck"),
    ("🐄", WHT, "cow"), ("🐮", WHT, "cowface"), ("🐴", BRN, "horse"), ("🐐", WHT, "goat"),
    ("🐑", WHT, "sheep"), ("🐖", PNK, "pig"), ("🐷", PNK, "pigface"), ("🐝", YEL, "bee"),
    ("🦌", BRN, "deer"), ("🐻", BRN, "bear"), ("🦊", ORA, "fox"), ("🐺", GRY, "wolf"),
    ("🐟", BLU, "fish"), ("🐍", GRN, "snake"), ("🦅", BRN, "eagle"), ("🐾", BRN, "paws"),
    # food & drink
    ("☕", BRN, "coffee"), ("🍺", YEL, "beer"), ("🍷", RED, "wine"), ("🍕", ORA, "pizza"),
    ("🍔", BRN, "burger"), ("🌮", YEL, "taco"), ("🍎", RED, "apple"), ("🍓", RED, "berry"),
    ("🥕", ORA, "carrot"), ("🍞", TAN, "bread"), ("🥓", RED, "bacon"), ("🍪", BRN, "cookie"),
    # misc symbols
    ("💔", RED, "broken"), ("💙", BLU, "blueheart"), ("💚", GRN, "greenheart"), ("💛", YEL, "yellowheart"),
    ("🖤", GRY, "blackheart"), ("✨", YEL, "sparkles"), ("💥", ORA, "boom"), ("💤", BLU, "zzz"),
    ("🎵", PUR, "music"), ("🏳", WHT, "whiteflag"), ("☠", WHT, "skull"), ("👻", WHT, "ghost"),
    ("🤖", GRY, "robot"), ("👽", GRN, "alien"), ("🚀", GRY, "rocket"), ("✈", GRY, "plane"),
]


def rgb565(c):
    r, g, b = c
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def render(font, ch, tofu):
    t = Image.new("L", (96, 96), 0)
    ImageDraw.Draw(t).text((8, 8), ch, font=font, fill=255)
    if tofu is not None and t.tobytes() == tofu:
        return None
    bb = t.getbbox()
    if not bb:
        return None
    t = t.crop(bb)
    t.thumbnail((16, 16), Image.LANCZOS)
    out = Image.new("L", (16, 16), 0)
    out.paste(t, ((16 - t.width) // 2, (16 - t.height) // 2))
    return out


def main():
    font_path = sys.argv[1] if len(sys.argv) > 1 else "NotoEmoji-Regular.ttf"
    font = ImageFont.truetype(font_path, 64)
    t = Image.new("L", (96, 96), 0)
    ImageDraw.Draw(t).text((8, 8), "\U000E0001", font=font, fill=255)   # surely missing: what tofu looks like
    tofu = t.tobytes()

    rows, missing = [], []
    for ch, col, name in SET:
        img = render(font, ch, tofu)
        if img is None:
            missing.append(ch)
            continue
        px = list(img.getdata()) if not hasattr(img, "get_flattened_data") else list(img.get_flattened_data())
        lv = [min(3, (p + 32) // 64) for p in px]   # 2-bit alpha
        data = bytearray()
        for i in range(0, 256, 4):
            data.append(lv[i] << 6 | lv[i + 1] << 4 | lv[i + 2] << 2 | lv[i + 3])
        rows.append((ord(ch), rgb565(col), name, bytes(data)))

    order = [r[0] for r in rows]           # picker order
    rows_sorted = sorted(rows, key=lambda r: r[0])
    index_of = {r[0]: i for i, r in enumerate(rows_sorted)}

    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "..", "..", "..", "examples", "companion_radio", "ui-cyberdeck", "EmojiData.h")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("// Generated by variants/lilygo_tdeck/tools/make_emoji.py - do not edit.\n")
        f.write("// Glyphs drawn from Noto Emoji, SIL Open Font License 1.1 (github.com/googlefonts/noto-emoji).\n")
        f.write("// 16x16, 2-bit alpha (4 pixels a byte, high bits first), one tint colour each.\n#pragma once\n#include <stdint.h>\n\n")
        f.write("struct EmojiGlyph { uint32_t cp; uint16_t color; const char* name; uint8_t a[64]; };\n\n")
        f.write(f"#define EMOJI_COUNT {len(rows_sorted)}\n")
        f.write("static const EmojiGlyph EMOJI[EMOJI_COUNT] = {   // sorted by code point\n")
        for cp, col, name, data in rows_sorted:
            f.write(f"  {{ 0x{cp:X}, 0x{col:04X}, \"{name}\", {{ {', '.join(str(b) for b in data)} }} }},\n")
        f.write("};\n\n")
        f.write("static const uint8_t EMOJI_PICKER[EMOJI_COUNT] = {   // indexes into EMOJI, in picker order\n  ")
        f.write(", ".join(str(index_of[cp]) for cp in order))
        f.write("\n};\n")
    print(f"wrote {len(rows)} emojis to {os.path.normpath(out)}")
    if missing:
        print("not in the font, left out:", " ".join(missing))


if __name__ == "__main__":
    main()
