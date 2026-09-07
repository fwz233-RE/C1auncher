#!/usr/bin/env python3
"""Kenney Interface Sounds (CC0) → C 数组音效表
输入: /tmp/kenney_iface.zip (OGG)
输出: src/platform/sfx_data.c  (48kHz int16 mono, 峰值归一 ±29000)

映射(游戏语义 → 素材):
  tick=click 移动反馈  tick_001, move=drop_001, select=select_001,
  clear=glass_001, error=error_001, win=maximize_001(上行), lose=minimize_001(下行)
"""
import zipfile, io, os
import soundfile as sf
import numpy as np

DIR = '/tmp/sfx/Audio'   # merge of kenney_ui.zip + kenney_iface.zip
OUT = 'src/platform/sfx_data.c'
SR = 48000
MAP = [
    ('tick',   'drop_002.ogg'),           # wood drop variant, same family as move
    ('move',   'drop_001.ogg'),           # wood drop (user liked)
    ('select', 'toggle_001.ogg'),         # rounded toggle (user OK)
    ('clear',  'pluck_001.ogg'),          # pluck, no glass (2048 approved)
    ('error',  'error_001.ogg'),
    ('win',    'maximize_001.ogg'),
    ('lose',   'minimize_001.ogg'),
]

blocks = []
total = 0
for name, path in MAP:
    data, sr = sf.read(open(os.path.join(DIR, path), 'rb'), dtype='float32', always_2d=True)
    mono = data.mean(axis=1)                    # L+R 混单
    # 重采样 sr→48000 (线性插值)
    n_out = int(round(len(mono) * SR / sr))
    src_x = np.linspace(0.0, len(mono) - 1, n_out)
    idx0 = src_x.astype(np.int32)
    idx1 = np.minimum(idx0 + 1, len(mono) - 1)
    frac = (src_x - idx0).astype(np.float32)
    res = mono[idx0] * (1 - frac) + mono[idx1] * frac
    # 时长上限: 高频反馈音太长会"拖尾", 裁剪尾部并加 2ms 淡出防爆音
    maxs = {'tick': 5000, 'move': 9000, 'select': 9000, 'clear': 12000,
            'error': 9000, 'win': 14000, 'lose': 14000}
    cap = maxs[name]
    if len(res) > cap:
        res = res[:cap]
        fade = min(96, cap // 4)
        for i in range(fade):
            res[cap - fade + i] = res[cap - fade + i] * float(i) / fade
    peak = np.max(np.abs(res))
    if peak < 1e-6:
        peak = 1.0
    res = res / peak * 12000.0                  # 峰值归一 ±12000(-8.3dB 余量, 防小喇叭过载)
    pcm = np.clip(res, -32768, 32767).astype(np.int16)
    blocks.append((name, pcm))
    total += len(pcm)
    print(f'{name:8s} {path:26s} {len(pcm):6d} samples ({len(pcm)/SR*1000:.0f}ms)')

with open(OUT, 'w') as f:
    f.write('/* Kenney Interface Sounds, CC0 1.0 (https://kenney.nl/assets/interface-sounds)\n')
    f.write(' * 48kHz int16 mono, 峰值归一. 由 tools/gen_sfx.py 生成 — 勿手改. */\n')
    f.write('#include <stdint.h>\n\n')
    for name, pcm in blocks:
        f.write(f'const int16_t g_sfx_{name}[] = {{\n')
        for i in range(0, len(pcm), 16):
            f.write('    ' + ','.join(str(v) for v in pcm[i:i+16]) + ',\n')
        f.write('};\n')
        f.write(f'const uint32_t g_sfx_{name}_len = {len(pcm)}u;\n\n')
print(f'total {total} samples = {total*2//1024} KiB rodata')
