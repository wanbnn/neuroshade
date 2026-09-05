"""Regenerate the embedded Hack 15px glyph atlas (Pillow and Hack-Regular.ttf)."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont
font = ImageFont.truetype('/usr/share/fonts/TTF/Hack-Regular.ttf',15)
rows=[]
for ch in range(32,127):
    im=Image.new('L',(10,20)); ImageDraw.Draw(im).text((0,0),chr(ch),font=font,fill=255)
    rows.append('{'+','.join(str(sum((1<<x) for x in range(10) if im.getpixel((x,y)) >= 96)) for y in range(20))+'}')
Path(__file__).with_name('glyphs.hpp').write_text('// Rasterized Hack font; see LICENSE and generate.py.\n#pragma once\n#include <cstdint>\nnamespace neuroshade::overlay { inline constexpr std::uint16_t glyphs[95][20] = {\n'+',\n'.join(rows)+'\n}; }\n')
