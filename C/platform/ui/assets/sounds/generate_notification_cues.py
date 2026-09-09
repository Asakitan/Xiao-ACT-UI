# Provenance: original procedural notification cues generated for SAO Auto.
# This script is the source of the four PCM WAV assets in this directory.
import math
import pathlib
import struct
import wave

ROOT = pathlib.Path(__file__).resolve().parent
RATE = 44100


def envelope(t, duration, attack=0.012, release=0.10):
    if t < attack:
        return t / attack
    if t > duration - release:
        return max(0.0, (duration - t) / release)
    return 1.0


def render(name, duration, tones, pulse_hz=0.0):
    frames = []
    count = int(RATE * duration)
    for index in range(count):
        t = index / RATE
        value = sum(math.sin(2.0 * math.pi * frequency * t + phase)
                    for frequency, phase, weight in tones for _ in range(weight))
        normalizer = max(1, sum(weight for _, _, weight in tones))
        if pulse_hz:
            value *= 0.52 + 0.48 * max(0.0, math.sin(2.0 * math.pi * pulse_hz * t))
        value *= envelope(t, duration) * 0.24 / normalizer
        sample = max(-1.0, min(1.0, value))
        encoded = int(sample * 32767)
        frames.extend((encoded, encoded))
    path = ROOT / name
    with wave.open(str(path), "wb") as output:
        output.setnchannels(2)
        output.setsampwidth(2)
        output.setframerate(RATE)
        output.writeframes(struct.pack("<%dh" % len(frames), *frames))


render("Notify.SAO.Message.wav", 0.30, [(880.0, 0.0, 1), (1320.0, 0.0, 1)])
render("Notify.SAO.System.wav", 0.50, [(523.25, 0.0, 1), (783.99, 0.0, 1), (1046.5, 0.0, 1)])
render("Notify.SAO.Warning.wav", 0.55, [(523.25, 0.0, 1), (622.25, 0.0, 1)], 7.0)
render("Notify.SAO.Emergency.wav", 0.75, [(392.0, 0.0, 1), (466.16, 0.0, 1)], 10.0)
