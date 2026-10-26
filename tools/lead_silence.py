"""Put silence in front of WAV files (any PCM format): PlaySound opens the audio device for every sound, and a device that was idle
(Bluetooth, HDMI, USB, power saving) needs a moment to wake up: the start of the sound is lost. A short silence in front takes that hit.
Usage: python tools/lead_silence.py <folder or file> [milliseconds, default 100]
Run it once on a sound as it comes from the sound editor: every run adds the silence again."""
import pathlib
import sys
import wave


def add_silence(path, ms):
    with wave.open(str(path), "rb") as r:
        params = r.getparams()
        data = r.readframes(r.getnframes())
    frames = int(params.framerate * ms / 1000)
    silence = bytes(frames * params.nchannels * params.sampwidth)
    with wave.open(str(path), "wb") as w:
        w.setparams(params)
        w.writeframes(silence + data)
    return frames


if __name__ == "__main__":
    target = pathlib.Path(sys.argv[1])
    ms = int(sys.argv[2]) if len(sys.argv) > 2 else 100
    files = [target] if target.is_file() else sorted(target.glob("*.wav"))
    for f in files:
        add_silence(f, ms)
        print("added %d ms of silence to %s" % (ms, f.name))
