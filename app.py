import io
import re
import wave
import asyncio
import socket
import traceback

import numpy as np
from scipy.signal import resample_poly
import miniaudio
import requests
import edge_tts
from flask import Flask, request, send_file, jsonify

try:
    from waitress import serve
    USE_WAITRESS = True
except ImportError:
    USE_WAITRESS = False

try:
    from faster_whisper import WhisperModel
    print("[STT] Loading faster-whisper model (base.en)...")
    whisper_model = WhisperModel("base.en", device="cpu", compute_type="int8")
    print("[STT] faster-whisper ready!")
    USE_WHISPER = True
except ImportError:
    print("[WARN] faster-whisper not installed. pip install faster-whisper")
    whisper_model = None
    USE_WHISPER = False

# ─── Config ──────────────────────────────────────────────────────────────────
OLLAMA_URL   = "http://localhost:11434/api/generate"
OLLAMA_MODEL = "llama3.2:1b"
SAMPLE_RATE  = 16000   # must match ESP32 firmware SAMPLE_RATE
TTS_VOICE    = "en-US-JennyNeural"   # change to any edge-tts voice you prefer
WAKE_WORD    = ""            # change to any phrase you like

app = Flask(__name__)

# ─── STT: WAV bytes → numpy → faster-whisper ─────────────────────────────────
def wav_bytes_to_numpy(wav_bytes: bytes) -> np.ndarray:
    with wave.open(io.BytesIO(wav_bytes)) as wf:
        ch, sw, sr, nf = wf.getnchannels(), wf.getsampwidth(), wf.getframerate(), wf.getnframes()
        raw_frames = wf.readframes(nf)
        samples = np.frombuffer(raw_frames, dtype=np.int16).astype(np.float32) / 32768.0
        if ch == 2:
            samples = samples.reshape(-1, 2).mean(axis=1)
    rms = np.sqrt(np.mean(samples ** 2))
    peak = np.max(np.abs(samples))
    dur  = len(samples) / sr
    print(f"[WAV] {sr}Hz  {ch}ch  {nf} frames  {dur:.2f}s  RMS={rms:.5f}  peak={peak:.5f}")
    # Normalize quiet ESP32 mic audio to a consistent RMS level.
    # INMP441 can record at very low amplitude; Whisper struggles with it.
    if rms > 1e-6:
        target_rms = 0.1
        gain = min(target_rms / rms, 20.0)   # raised cap: 20× to handle very quiet mics
        samples = np.clip(samples * gain, -1.0, 1.0)
        print(f"[WAV] Applied gain ×{gain:.1f}  → RMS={np.sqrt(np.mean(samples**2)):.5f}")
    else:
        print("[WAV] Audio is near-silent (RMS≈0) — mic may not be recording")
    return samples

def transcribe(wav_bytes: bytes) -> str:
    if not USE_WHISPER or not wav_bytes:
        return ""
    try:
        audio_np = wav_bytes_to_numpy(wav_bytes)
        segments, info = whisper_model.transcribe(
            audio_np,
            beam_size=5,
            temperature=0.0,
            language="en",
            vad_filter=True,
            vad_parameters=dict(
                min_silence_duration_ms=300,
                speech_pad_ms=400,        # pad detected speech so words aren't clipped
            ),
            initial_prompt="Transcription of clear English speech spoken to a voice assistant.",
            condition_on_previous_text=False,
            compression_ratio_threshold=2.4,  # reject repetitive/looping hallucinations
            log_prob_threshold=-1.0,          # reject very uncertain segments
            no_speech_threshold=0.7,          # allow segments up to 0.7 no-speech prob through
        )
        # Strip transcription tags like [coughs] or (sighs)
        seg_list = list(segments)
        print(f"[STT] {len(seg_list)} segment(s)  duration={info.duration:.2f}s  vad_duration={info.duration_after_vad:.2f}s")
        for s in seg_list:
            print(f"  seg [{s.start:.1f}s→{s.end:.1f}s] no_speech={s.no_speech_prob:.3f}: \"{s.text.strip()}\"")
        text = " ".join(s.text.strip() for s in seg_list).strip()
        text = re.sub(r'\[.*?\]|\(.*?\)', '', text).strip()
        
        # Only block phrases that are pure Whisper hallucinations on silence/noise.
        # Do NOT block real words a user might actually say to a voice assistant.
        HALLUCINATIONS = {
            "thanks for watching", "thanks for watching.", "thank you for watching",
            "please subscribe", "subscribe", "subscribe.", "like and subscribe",
        }
        text_lower = text.lower().strip()
        if text_lower in HALLUCINATIONS or len(text_lower) <= 1:
            print(f"[STT] Ignored unwanted/hallucinated sound: \"{text}\"")
            return ""
            
        print(f"[STT] ({info.language}): \"{text}\"")
        return text
    except Exception as e:
        print(f"[STT] Error: {e}")
        return ""

# ─── TTS: edge-tts (neural) → miniaudio decode → normalized WAV bytes ────────
async def _edge_tts_mp3(text: str) -> bytes:
    """Fetch MP3 audio from edge-tts neural voice."""
    communicate = edge_tts.Communicate(text, voice=TTS_VOICE)
    chunks = []
    async for chunk in communicate.stream():
        if chunk["type"] == "audio":
            chunks.append(chunk["data"])
    return b"".join(chunks)

def tts_to_wav_bytes(text: str) -> bytes:
    """
    edge-tts (24 kHz 48kbps MP3)
      → miniaudio: decode MP3 at native 24 kHz (no resampling here)
      → scipy resample_poly 24 kHz → 16 kHz  (polyphase anti-aliased, clean)
      → gentle fixed gain → clip → WAV bytes
    """
    NATIVE_RATE = 24000   # edge-tts always outputs 24 kHz MP3

    # 1. Get MP3 bytes from edge-tts
    mp3_bytes = asyncio.run(_edge_tts_mp3(text))

    # 2. Decode MP3 at its native 24 kHz — no resampling yet, preserves quality
    decoded = miniaudio.decode(
        mp3_bytes,
        output_format=miniaudio.SampleFormat.SIGNED16,
        nchannels=1,
        sample_rate=NATIVE_RATE,
    )

    # 3. Resample 24 kHz → 16 kHz with scipy polyphase filter (ratio 2:3).
    #    resample_poly applies a proper anti-aliasing FIR filter — far cleaner
    #    than miniaudio's built-in linear interpolation resampler.
    samples = np.frombuffer(decoded.samples, dtype=np.int16).astype(np.float32)
    samples = resample_poly(samples, up=2, down=3)   # 24000 * 2/3 = 16000

    # 4. Peak normalize to 40% volume. The MAX98357A is a 3W amp and gets
    #    extremely loud. Pushing it past 50% often causes physical speaker distortion.
    max_val = np.max(np.abs(samples))
    if max_val > 0:
        samples = samples * ((32767.0 * 0.4) / max_val)
    samples = samples.astype(np.int16)

    # 5. Pack into WAV
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(samples.tobytes())
    wav_bytes = buf.getvalue()
    print(f"[TTS] WAV ready: {len(wav_bytes):,} bytes  voice={TTS_VOICE}")
    return wav_bytes

# ─── Health check ─────────────────────────────────────────────────────────────
@app.route('/ping', methods=['GET'])
def ping():
    return jsonify({
        "status": "ok",
        "model": OLLAMA_MODEL,
        "stt": "faster-whisper base.en" if USE_WHISPER else "off",
        "tts": f"edge-tts {TTS_VOICE}",
    })

# ─── Main voice endpoint ───────────────────────────────────────────────────────
@app.route('/chat', methods=['POST'])
def chat():
    print("\n" + "=" * 50)
    print("  Query received from ESP32")
    print("=" * 50)

    try:
        # 1. Read incoming WAV bytes
        audio_bytes = request.get_data()
        if not audio_bytes or len(audio_bytes) < 44:
            return "Audio payload too small", 400
        print(f"[1/4] Audio received: {len(audio_bytes):,} bytes")

        # 2. Speech-to-Text
        text = transcribe(audio_bytes)
        print(f"[2/4] Query: \"{text}\"")

        # 2b. Wake-word gate — supports comma-separated list e.g. "abi,bi,be"
        #     Disable entirely by setting WAKE_WORD = ""
        text_lower = text.lower().strip()
        matched_word = None
        if WAKE_WORD.strip():
            wake_words = [w.strip().lower() for w in WAKE_WORD.split(",") if w.strip()]
            matched_word = next((w for w in wake_words if text_lower.startswith(w)), None)
            if not matched_word:
                print(f"[GATE] No wake word detected — ignoring")
                return "", 204   # ESP32 sees 204 and goes back to Listening silently

        # Strip matched wake word from query before sending to LLM
        text = text[len(matched_word):].strip(" ,.") if matched_word else text
        print(f"[GATE] Wake word '{matched_word}' matched — query: \"{text}\"")

        # 3. Ollama LLM
        if text:
            try:
                print(f"[3/4] Querying {OLLAMA_MODEL}...")
                resp = requests.post(
                    OLLAMA_URL,
                    json={
                        "model": OLLAMA_MODEL,
                        "prompt": (
                            "You are a helpful, concise voice assistant. "
                            "Reply in 1-2 short spoken sentences only, no markdown: "
                            + text
                        ),
                        "stream": False,
                    },
                    timeout=60,
                )
                resp.raise_for_status()
                reply = resp.json().get("response", "").strip()
                reply = re.sub(r"[*#`\-]", "", reply)
                reply = reply.split("\n")[0].strip()
                if not reply:
                    reply = "I'm not sure about that."
                print(f"[3/4] Reply: \"{reply}\"")
            except Exception as e:
                print(f"[3/4] Ollama error: {e}")
                reply = "I'm having trouble reaching my brain right now."
        else:
            reply = "I didn't catch that, could you try again?"

        # 4. TTS
        print(f"[4/4] TTS: \"{reply[:70]}\"")
        wav_bytes = tts_to_wav_bytes(reply)

        print("[OK] Sending response to ESP32\n")
        resp = send_file(io.BytesIO(wav_bytes), mimetype="audio/wav", as_attachment=False)
        # Send reply text as header so ESP32 can show it on the OLED
        safe = reply[:80].encode("ascii", errors="replace").decode("ascii")
        resp.headers["X-Reply-Text"] = safe
        return resp

    except Exception:
        print("[ERROR] Unhandled exception in /chat:")
        traceback.print_exc()
        return "Internal server error", 500

# ─── Entry point ──────────────────────────────────────────────────────────────
if __name__ == "__main__":
    try:
        _s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        _s.connect(("8.8.8.8", 80))
        local_ip = _s.getsockname()[0]
        _s.close()
    except Exception:
        local_ip = "unknown"

    print("\n" + "=" * 50)
    print("  ESP32-S3 × Ollama Voice Assistant")
    print(f"  STT  : faster-whisper base.en")
    print(f"  LLM  : {OLLAMA_MODEL} via Ollama")
    print(f"  TTS  : edge-tts  {TTS_VOICE}")
    print(f"  IP   : {local_ip}:5000")
    print(f"  URL  : http://{local_ip}:5000/chat")
    print("=" * 50 + "\n")

    if USE_WAITRESS:
        print("Serving with Waitress (production)...")
        serve(app, host="0.0.0.0", port=5000, threads=2)
    else:
        app.run(host="0.0.0.0", port=5000, debug=False, threaded=True)
