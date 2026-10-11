#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""纯标准库 WAV 分析:输出能量包络与基频轨迹,用于手工克隆音效。

用法: python analyze_wav.py <file.wav> [窗口ms]
"""
import sys
import wave
import array
import math


def read_mono(path):
    with wave.open(path, 'rb') as w:
        nch = w.getnchannels()
        sw = w.getsampwidth()
        sr = w.getframerate()
        n = w.getnframes()
        raw = w.readframes(n)
    if sw != 2:
        raise SystemExit('只支持 16-bit PCM, 实际 %d 字节' % sw)
    data = array.array('h')
    data.frombytes(raw)
    if nch == 1:
        return sr, data
    mono = array.array('h', [0]) * 0
    mono = array.array('h')
    for i in range(0, len(data) - nch + 1, nch):
        acc = 0
        for c in range(nch):
            acc += data[i + c]
        mono.append(acc // nch)
    return sr, mono


def autocorr_freq(seg, sr):
    """自相关估基频:在 120..5000 Hz 范围内找最大相关峰。"""
    n = len(seg)
    if n < 64:
        return 0
    lo = int(sr / 5000)
    hi = min(int(sr / 120), n - 1)
    if hi <= lo:
        return 0
    best_lag, best_val = 0, 0.0
    mean = sum(seg) / float(n)
    var = sum((x - mean) ** 2 for x in seg)
    if var <= 0:
        return 0
    for lag in range(lo, hi + 1):
        s = 0.0
        for i in range(n - lag):
            s += (seg[i] - mean) * (seg[i + lag] - mean)
        s /= (n - lag)
        score = s / var
        if score > best_val:
            best_val, best_lag = score, lag
    if best_lag == 0 or best_val < 0.08:
        return 0
    return sr / float(best_lag)


def zc_freq(seg, sr):
    """零穿越率估频:对 pluck 类音色比自相关稳。"""
    n = len(seg)
    if n < 32:
        return 0
    mean = sum(seg) / float(n)
    crossings = 0
    prev = seg[0] - mean
    for i in range(1, n):
        cur = seg[i] - mean
        if (prev <= 0 < cur) or (cur <= 0 < prev):
            crossings += 1
        prev = cur
    return crossings * sr / (2.0 * n)


def main():
    path = sys.argv[1]
    win_ms = int(sys.argv[2]) if len(sys.argv) > 2 else 10
    sr, data = read_mono(path)
    print('file=%s  sr=%d  samples=%d  dur=%.3fs' % (path, sr, len(data), len(data) / float(sr)))

    win = max(1, int(sr * win_ms / 1000.0))
    peak = max(1, max(abs(x) for x in data))
    print('peak=%d' % peak)
    print('%-8s %-6s %-10s %-10s %s' % ('t_ms', 'rms%', 'ac_Hz', 'zc_Hz', 'bar'))
    for w in range(0, len(data) - win + 1, win):
        seg = data[w:w + win]
        rms = math.sqrt(sum(float(x) * x for x in seg) / len(seg))
        pct = rms / peak * 100.0
        if pct < 2.0:
            continue
        fa = autocorr_freq(seg, sr)
        fz = zc_freq(seg, sr)
        bar = '#' * int(min(pct, 100) / 2)
        print('%-8d %-6.1f %-10.0f %-10.0f %s' % (w * 1000 // sr, pct, fa, fz, bar))


if __name__ == '__main__':
    main()
