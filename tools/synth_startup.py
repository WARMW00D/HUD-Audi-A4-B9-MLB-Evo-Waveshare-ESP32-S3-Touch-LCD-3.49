#!/usr/bin/env python3
"""
synth_startup.py — синтез стартовых звуков HUD с нуля (без чужих сэмплов).
Звук целиком создаётся кодом, права на результат — у автора проекта.
Выход: startup_<вариант>.wav (44.1 кГц моно) в текущей папке. Нужный вариант
скопировать в tools/sounds/startup.wav и запустить tools/make_sounds.py.
Запуск: python tools/synth_startup.py   (нужен numpy)
"""
import numpy as np, wave, os

SR = 44100

def env(n, a=0.005, d=1.2):
    t = np.arange(n) / SR
    e = np.minimum(1.0, t / a) * np.exp(-t / d)
    return e

def bell(f, dur, decay=0.9, bright=1.0):
    """Колокольчик: FM-синтез (несущая f, модулятор 3.5f, индекс затухает)."""
    n = int(SR * dur); t = np.arange(n) / SR
    idx = bright * 2.2 * np.exp(-t / (decay * 0.35))
    s = np.sin(2 * np.pi * f * t + idx * np.sin(2 * np.pi * 3.5 * f * t))
    s += 0.25 * np.sin(2 * np.pi * 2 * f * t) * np.exp(-t / (decay * 0.5))
    return s * env(n, 0.004, decay)

def pad(f, dur):
    """Мягкий пэд: две слегка расстроенные пилы через ФНЧ (скользящее среднее)."""
    n = int(SR * dur); t = np.arange(n) / SR
    s = sum(((2 * ((ff * t) % 1) - 1)) for ff in (f, f * 1.004, f * 0.996)) / 3
    k = 40
    s = np.convolve(s, np.ones(k) / k, mode="same")
    att = np.minimum(1.0, t / 0.35)
    rel = np.clip((dur - t) / 0.6, 0, 1)
    return s * att * rel

def mix(parts, total):
    out = np.zeros(int(SR * total))
    for start, sig, gain in parts:
        i = int(SR * start)
        out[i:i + len(sig)] += gain * sig[:len(out) - i]
    return out

def reverb(x, mix_=0.25):
    """Простая реверберация: несколько затухающих задержек."""
    y = x.copy()
    for d, g in ((0.031, 0.5), (0.047, 0.4), (0.071, 0.33), (0.113, 0.25), (0.167, 0.18)):
        k = int(SR * d)
        z = np.zeros_like(x); z[k:] = x[:-k] * g
        y += z * mix_ * 2
    return y

def save(name, x):
    x = x / (np.max(np.abs(x)) + 1e-9) * 0.9
    fade = int(SR * 0.03); x[-fade:] *= np.linspace(1, 0, fade)
    d = (x * 32767).astype(np.int16)
    with wave.open(name, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR); w.writeframes(d.tobytes())
    print(name, f"{len(d) / SR:.2f} c")

N = lambda semis: 440.0 * 2 ** (semis / 12)          # 0 = A4
C5, E5, G5, C6, D5, A5, E6, G6 = N(3), N(7), N(10), N(15), N(5), N(12), N(19), N(22)

# 1) «Арпеджио»: восходящее мажорное трезвучие колокольчиками + мягкий пэд
a = mix([(0.00, bell(C5, 1.6), 0.8), (0.11, bell(E5, 1.5), 0.75),
         (0.22, bell(G5, 1.4), 0.7), (0.33, bell(C6, 1.6, 1.1), 0.8),
         (0.00, pad(C5 / 2, 1.9), 0.18)], 2.0)
save("startup_arpeggio.wav", reverb(a))

# 2) «Двойной сигнал»: короткий, спокойный — две ноты (кварта вверх)
b = mix([(0.00, bell(E5, 1.1, 0.7, 0.8), 0.9), (0.16, bell(A5, 1.3, 0.8, 0.8), 0.9)], 1.4)
save("startup_double.wav", reverb(b, 0.2))

# 3) «Включение»: восходящий свип пэда + яркий аккорд колокольчиков в конце
n = int(SR * 1.0); t = np.arange(n) / SR
f = 220 * 2 ** (t * 1.0)                                   # октава вверх за 1 с
sweep = np.sin(2 * np.pi * np.cumsum(f) / SR) * np.minimum(1, t / 0.2) * 0.5
c = mix([(0.0, sweep, 0.5),
         (0.85, bell(C6, 1.4, 1.0), 0.6), (0.85, bell(E6, 1.4, 1.0), 0.5), (0.85, bell(G6, 1.4, 1.0), 0.45),
         (0.6, pad(C5, 1.6), 0.2)], 2.3)
save("startup_powerup.wav", reverb(c))
