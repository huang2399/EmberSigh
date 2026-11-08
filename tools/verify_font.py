# -*- coding: utf-8 -*-
"""
验证生成的字库：完全按 LVGL lv_font_fmt_txt.c 的 4bpp PLAIN 读取逻辑解码字形，
渲染成 PNG，肉眼确认「听眠/环境静谧/正在恢复」是否真的能显示。
"""
import io, re, os
from PIL import Image

C_FILE = r'C:\Users\27568\Desktop\DreamState\EmberSigh\main\fonts\embersigh_fonts.c'
OUT_PNG = r'C:\Users\27568\Desktop\DreamState\EmberSigh\tools\font_preview.png'

s = io.open(C_FILE, encoding='utf-8').read()


def grab_array(name, elem='uint8_t'):
    m = re.search(r'static const ' + elem + r'\s+' + name + r'\[\]\s*=\s*\{', s)
    if not m:
        return None
    i = m.end()
    depth = 1
    while i < len(s) and depth > 0:
        if s[i] == '{':
            depth += 1
        elif s[i] == '}':
            depth -= 1
        i += 1
    body = s[m.end():i-1]
    return [int(x, 0) for x in re.findall(r'0[xX][0-9a-fA-F]+|\b\d+\b', body)]


def grab_glyph_dsc(tag):
    m = re.search(r'static const lv_font_fmt_txt_glyph_dsc_t\s+glyph_dsc_' + str(tag) + r'\[\]\s*=\s*\{', s)
    i = m.end()
    depth = 1
    while i < len(s) and depth > 0:
        if s[i] == '{':
            depth += 1
        elif s[i] == '}':
            depth -= 1
        i += 1
    body = s[m.end():i-1]
    out = []
    for em in re.finditer(
            r'\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), '
            r'\.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)', body):
        out.append(dict(bitmap_index=int(em.group(1)), adv_w=int(em.group(2)),
                        box_w=int(em.group(3)), box_h=int(em.group(4)),
                        ofs_x=int(em.group(5)), ofs_y=int(em.group(6))))
    return out


def grab_cmaps(tag):
    m = re.search(r'static const lv_font_fmt_txt_cmap_t\s+cmaps_' + str(tag) + r'\[\]\s*=\s*\{', s)
    i = m.end()
    depth = 1
    while i < len(s) and depth > 0:
        if s[i] == '{':
            depth += 1
        elif s[i] == '}':
            depth -= 1
        i += 1
    body = s[m.end():i-1]
    out = []
    blocks = re.split(r'(?=\.range_start\s*=)', body)
    for b in blocks:
        if '.range_start' not in b:
            continue
        out.append(dict(
            start=int(re.search(r'\.range_start\s*=\s*(\d+)', b).group(1)),
            length=int(re.search(r'\.range_length\s*=\s*(\d+)', b).group(1)),
            gid_start=int(re.search(r'\.glyph_id_start\s*=\s*(\d+)', b).group(1)),
            ulist=re.search(r'\.unicode_list\s*=\s*(\w+)', b).group(1),
            listlen=int(re.search(r'\.list_length\s*=\s*(\d+)', b).group(1)),
        ))
    return out


def decode_glyph(bitmap, gd, bpp=4):
    """严格复刻 LVGL 的连续 nibble 读取（stride_in = 0）"""
    w, h = gd['box_w'], gd['box_h']
    if w == 0 or h == 0:
        return None
    img = Image.new('L', (w, h), 0)
    px = img.load()
    idx = gd['bitmap_index']
    shift = 8
    cur = 0
    for y in range(h):
        x = 0
        if shift == 4:
            px[x, y] = (cur & 0xF) * 17
            x += 1
            shift = 8
        while x + 1 < w:
            cur = bitmap[idx]
            idx += 1
            px[x, y] = (cur >> 4) * 17
            px[x + 1, y] = (cur & 0xF) * 17
            x += 2
        if x < w:
            cur = bitmap[idx]
            idx += 1
            px[x, y] = (cur >> 4) * 17
            shift = 4
    return img


def lookup(unicode_list, cmaps, cp):
    for c in cmaps:
        if c['listlen'] == 0:
            if c['start'] <= cp < c['start'] + c['length']:
                return c['gid_start'] + (cp - c['start'])
        else:
            off = cp - c['start']
            if off in unicode_list:
                return c['gid_start'] + unicode_list.index(off)
    return None


# ---- 渲染预览 ----
SAMPLE16 = "听眠"
SAMPLE14 = "环境静谧正在恢复"

panels = []
for tag, sample, size in ((0, SAMPLE16, 16), (1, SAMPLE14, 14)):
    bitmap = grab_array(f'glyph_bitmap_{tag}')
    gdsc = grab_glyph_dsc(tag)
    cmaps = grab_cmaps(tag)
    ulist = grab_array(f'unicode_list_{tag}', 'uint16_t') or []

    # 以基线与 ofs_y 还原到统一画布
    CELL = max(size, 16) + 12
    W = CELL * len(sample)
    H = CELL + 6
    canvas = Image.new('L', (W, H), 0)

    pen = 0
    ok = 0
    for ch in sample:
        cp = ord(ch)
        gid = lookup(ulist, cmaps, cp)
        if gid is None:
            print(f'  {ch} U+{cp:04X} 查表失败')
            continue
        g = gdsc[gid]
        gimg = decode_glyph(bitmap, g)
        if gimg is None:
            print(f'  {ch} 空字形 (box 0)')
            continue
        # 基线固定在 y=CELL-4；字形顶部 = 基线 - (line_height - base_line - ofs_y)
        # 用简化方式：top = CELL - 4 - (ascent - ofs_y)，ascent 取 box_h+ofs_y
        top = (CELL - 6) - (size - g['ofs_y'])
        canvas.paste(gimg, (pen + g['ofs_x'], top))
        pen += max(1, g['adv_w'] // 16)
        ok += 1

    print(f'字号 {size}: 渲染 {ok}/{len(sample)} 个字, 位图 {len(bitmap)}B')
    panels.append(canvas)

# 拼成一张图（放大 4 倍便于查看）
gap = 8
W = max(p.width for p in panels)
H = sum(p.height for p in panels) + gap * (len(panels) - 1)
out = Image.new('L', (W, H), 0)
y = 0
for p in panels:
    out.paste(p, (0, y))
    y += p.height + gap
out = out.resize((W * 4, H * 4), Image.NEAREST)
out.save(OUT_PNG)
print(f'\n预览已保存: {OUT_PNG}')
