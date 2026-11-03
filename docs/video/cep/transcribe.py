"""Write word timings for voice.wav to words.json as [[start, end, word], ...]."""
import json
import subprocess

import numpy as np
from faster_whisper import WhisperModel

raw = subprocess.run(["ffmpeg", "-v", "error", "-i", "voice.wav", "-f", "s16le", "-ac", "1", "-ar", "16000", "-"], capture_output=True, check=True).stdout
audio = np.frombuffer(raw, np.int16).astype(np.float32) / 32768

model = WhisperModel("small.en", device="cpu", compute_type="int8", cpu_threads=6, local_files_only=True)
segments, info = model.transcribe(audio, word_timestamps=True, beam_size=5, language="en", vad_filter=False)
words = []
for s in segments:
    for w in s.words:
        words.append([round(w.start, 3), round(w.end, 3), w.word.strip()])
json.dump(words, open("words.json", "w", encoding="utf-8"))
print(len(words), "words,", round(info.duration, 2), "s")
print(" ".join(w[2] for w in words))
