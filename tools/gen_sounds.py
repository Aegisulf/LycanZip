"""Makes the "Wave" sound set (simple sine-wave tones) in app/Assets/Sounds/Wave. The "Synth" set is made by hand (the sound list).
Every sound starts with LEAD_MS of silence: PlaySound opens the audio device for each sound, and a device that was idle needs a moment
to wake up, which would eat the start of the sound (see tools/lead_silence.py).
Usage: python tools/gen_sounds.py
"""
import math, struct, wave, pathlib

OUT = pathlib.Path(__file__).resolve().parent.parent / "app" / "Assets" / "Sounds" / "Wave"
RATE = 44100
LEAD_MS = 100


def tone(notes, vol=0.35):
    """notes: [(frequency Hz, seconds)]; each note fades in and out to avoid clicks."""
    data = []
    for f, d in notes:
        n = int(RATE * d)
        for i in range(n):
            t = i / RATE
            env = min(1.0, i / (RATE * 0.01), (n - i) / (RATE * 0.06))
            s = math.sin(2 * math.pi * f * t) + 0.3 * math.sin(2 * math.pi * f * 2 * t)
            data.append(int(32767 * vol * env * s / 1.3))
    return data


SOUNDS = {
    "open": [(660, 0.07), (880, 0.12)],                      # open an archive
    "start": [(520, 0.08)],                                  # task start
    "done": [(660, 0.09), (830, 0.09), (1050, 0.2)],         # task done
    "error": [(300, 0.14), (220, 0.26)],                     # failure
    "conflict": [(740, 0.08), (740, 0.08)],                  # needs a decision of the user (conflict / password)
    "drop": [(440, 0.06), (587, 0.1)],                       # a dragged file was dropped
}

OUT.mkdir(parents=True, exist_ok=True)
for name, notes in SOUNDS.items():
    with wave.open(str(OUT / f"{name}.wav"), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", s) for s in [0] * (RATE * LEAD_MS // 1000) + tone(notes)))
    print("wrote", name + ".wav")
