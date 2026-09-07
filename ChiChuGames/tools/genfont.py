#!/usr/bin/env python3
"""genfont.py — 从 Adafruit glcdfont.c（BSD 许可 5x7 字体）生成行式字形数据。
许可与来源见 ../THIRD_PARTY_NOTICES.md 和 ../licenses/Adafruit-GFX-BSD.txt。

输出:
  src/gfx/font_gen.h    — 字体参数 + 符号索引常量
  src/gfx/font_data.c   — ASCII 5x7 字形(95) + 符号字形(16) + CJK 12x12 子集(占位, M3)

字形编码: 每字形 7 字节, 每字节一行, bit0=最左像素, bit 置 1 = 黑。
5x7 字体由 glcdfont.c 的列式 5x8(取上 7 行)转置得到。
符号表为手绘 7 行 x 5 位字符串表('#'=黑)。
运行: python3 tools/genfont.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------- 5x7 ASCII
def load_glcdfont():
    path = os.path.join(ROOT, "tools", "glcdfont.c")
    data = open(path, encoding="utf-8").read()
    bytes_ = [int(b, 16) for b in re.findall(r"0x([0-9A-Fa-f]{2})", data)]
    if len(bytes_) < 256 * 5:
        sys.exit(f"glcdfont.c: 期望 >= {256*5} 字节, 实际 {len(bytes_)}")
    bytes_ = bytes_[: 256 * 5]  # 256 字形, 按 ASCII 码直接索引(0-255)
    glyphs = []  # 每字形 7 字节行式
    for i in range(256):
        col = bytes_[i * 5 : i * 5 + 5]  # 每字节一列
        rows = []
        for r in range(7):
            v = 0
            for x in range(5):
                if col[x] & (1 << r):  # 实测: Adafruit 字形 bit0=顶行!
                    v |= 1 << x
            rows.append(v)
        glyphs.append(rows)
    return glyphs

# ---------------------------------------------------------------- 符号表
# 7 行 x 5 位; '#'=黑。索引对应 font_gen.h 的 CG_* 常量。
SYMBOLS = {
    "ARROW_UP":    ["..#..", ".###.", "#####", "#...#", "#...#", "#...#", "#...#"],
    "ARROW_DN":    ["#...#", "#...#", "#...#", "#...#", "#####", ".###.", "..#.."],
    "ARROW_LT":    ["#....", "##...", "###..", "####.", "###..", "##...", "#...."],
    "ARROW_RT":    ["....#", "...##", "..###", ".####", "..###", "...##", "....#"],
    "SQUARE_FILL": ["#####", "#####", "#####", "#####", "#####", "#####", "#####"],
    "SQUARE_OPEN": ["#####", "#...#", "#...#", "#...#", "#...#", "#...#", "#####"],
    "FLAG":        ["#####", "#....", "#....", "#####", "#....", "#....", "#...."],
    "MINE":        ["...#.", "...#.", "..##.", ".###.", "#####", "###..", "##..."],
    "STAR":        ["..#..", "..#..", "#####", "#####", ".#.#.", ".#.#.", ".#.#."],
    "CHECK":       [".....", "....#", "...#.", "##.#.", "#.#..", ".#...", "....."],
    "CROSS":       ["#...#", "##..#", ".#.#.", "..#..", ".#.#.", "##..#", "#...#"],
    "HEART":       [".##.#", "#.#.#", "#.#.#", "#...#", "#...#", ".#.#.", "..#.."],
    "DIAMOND":     ["..#..", ".#.#.", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    "SPADE":       ["..#..", ".#.#.", "#...#", "#...#", "#####", ".#.#.", ".#.#."],
    "CLUB":        ["..#..", "..#..", ".##..", "####.", "#.#.#", "..#..", "..#.."],
    "RING":        [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
}

SYMBOL_NAMES = [
    "ARROW_UP", "ARROW_DN", "ARROW_LT", "ARROW_RT",
    "SQUARE_FILL", "SQUARE_OPEN", "FLAG", "MINE",
    "STAR", "CHECK", "CROSS", "HEART",
    "DIAMOND", "SPADE", "CLUB", "RING",
]

def parse_symbol(rows):
    out = []
    for line in rows:
        v = 0
        for x, ch in enumerate(line):
            if ch == "#":
                v |= 1 << x
        out.append(v)
    return out

# ---------------------------------------------------------------- 生成
def emit_array(name, glyphs, size):
    lines = [f"const uint8_t {name}[{size}][7] = {{"]
    for g in glyphs:
        lines.append("    {" + ", ".join(f"0x{v:02x}" for v in g) + "},")
    lines.append("};")
    return "\n".join(lines)

def main():
    ascii_glyphs = load_glcdfont()
    sym_glyphs = [parse_symbol(SYMBOLS[n]) for n in SYMBOL_NAMES]

    header = [
        "/* 自动生成: tools/genfont.py — 请勿手改 */",
        "#ifndef FONT_GEN_H",
        "#define FONT_GEN_H",
        "#include <stdint.h>",
        "#define FONT_W 5",
        "#define FONT_H 7",
        "#define FONT_ADV 6            /* 5+1 间距 */",
        "#define FONT_CJK_W 12",
        "#define FONT_CJK_H 12",
        "#define FONT_CJK_ADV 12",
        "#define CG_ARROW_UP 0",
        "#define CG_ARROW_DN 1",
        "#define CG_ARROW_LT 2",
        "#define CG_ARROW_RT 3",
        "#define CG_SQUARE_FILL 4",
        "#define CG_SQUARE_OPEN 5",
        "#define CG_FLAG 6",
        "#define CG_MINE 7",
        "#define CG_STAR 8",
        "#define CG_CHECK 9",
        "#define CG_CROSS 10",
        "#define CG_HEART 11",
        "#define CG_DIAMOND 12",
        "#define CG_SPADE 13",
        "#define CG_CLUB 14",
        "#define CG_RING 15",
        "#define CG_COUNT 16",
        "/* CJK 子集占位: 0 表示未生成(M3 由 genfont.py --cjk 生成) */",
        "extern const uint8_t font_cjk_count;",
        "extern const uint16_t font_cjk_codes[];",
        "extern const uint8_t font_cjk_data[];",
        "extern const uint8_t font_glyph5x7[256][7];",
        "extern const uint8_t font_symbols[CG_COUNT][7];",
        "#endif",
    ]

    data = [
        "/* 自动生成: tools/genfont.py — 请勿手改 */",
        "#include \"font_gen.h\"",
        "",
        emit_array("font_glyph5x7", ascii_glyphs, 256),
        "",
        emit_array("font_symbols", sym_glyphs, len(sym_glyphs)),
        "",
        "/* CJK 12x12 子集(M3 启用): 空表 */",
        "const uint8_t font_cjk_count = 0;",
        "const uint16_t font_cjk_codes[1] = {0};",
        "const uint8_t font_cjk_data[1] = {0};",
        "",
    ]

    with open(os.path.join(ROOT, "src", "gfx", "font_gen.h"), "w", encoding="utf-8") as f:
        f.write("\n".join(header) + "\n")
    with open(os.path.join(ROOT, "src", "gfx", "font_data.c"), "w", encoding="utf-8") as f:
        f.write("\n".join(data) + "\n")
    print(f"OK: {len(ascii_glyphs)} ASCII + {len(sym_glyphs)} symbols")

if __name__ == "__main__":
    main()
