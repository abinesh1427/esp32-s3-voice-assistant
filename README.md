# esp32-ollama

> Talk to a local LLM from an ESP32-S3. Whisper for ears, Ollama for brains, edge-tts for voice. Zero cloud, zero subscriptions.

Wake-word triggered voice assistant on an **ESP32-S3** microcontroller. Speak to it, it thinks with a local LLM, and talks back in a natural neural voice — all on your own hardware.

---

## How it works

```
[ESP32-S3]                          [Python server on PC]

  INMP441 mic
      │  I2S @ 16 kHz
      ▼
  Window RMS VAD
      │  wake word heard?
      ▼
  Record 4 s WAV
      │  HTTP POST /chat
      ▼──────────────────────────► faster-whisper tiny.en  (local STT)
                                          │  wake-word gate
                                          ▼
                                    Ollama llama3.2:1b  (local LLM)
                                          │
                                          ▼
                                    edge-tts neural voice  (online TTS)
                                    scipy polyphase resample 24→16 kHz
                                          │  16 kHz mono WAV + X-Reply-Text header
      ◄─────────────────────────────────────
      │
  MAX98357A amp → Speaker
  SSD1306 OLED  → scrolling reply text
```

> STT and LLM run fully **offline**. edge-tts requires internet to reach Microsoft neural TTS servers.

---

## Hardware

| Module | Pin | ESP32-S3 GPIO |
|---|---|---|
| SSD1306 OLED 128×64 | VCC | 3V3 |
| SSD1306 OLED | GND | GND |
| SSD1306 OLED | SDA | GPIO 1 |
| SSD1306 OLED | SCL | GPIO 2 |
| INMP441 Mic | VCC | 3V3 |
| INMP441 Mic | GND | GND |
| INMP441 Mic | WS | GPIO 15 |
| INMP441 Mic | SCK | GPIO 16 |
| INMP441 Mic | SD | GPIO 17 |
| INMP441 Mic | L/R | GND |
| MAX98357A Amp | VIN | 5V |
| MAX98357A Amp | GND | GND |
| MAX98357A Amp | DIN | GPIO 18 |
| MAX98357A Amp | BCLK | GPIO 8 |
| MAX98357A Amp | LRC | GPIO 9 |
| Speaker | + | SPK+ |
| Speaker | − | SPK− |
| Red LED + 330 Ω | Anode | GPIO 10 |
| Red LED | Cathode | GND |
| Green LED + 330 Ω | Anode | GPIO 11 |
| Green LED | Cathode | GND |

---

## OLED states

| Display | LED | Meaning |
|---|---|---|
| `Starting..` | — | Boot / WiFi connecting |
| `Listening` | Green | Idle, waiting for voice |
| `Recording` | Red | Mic active |
| `Thinking` | Red | Waiting for server |
| `Speaking` + scrolling text | — | Playing response |
| `Error` | — | Server error |

---

## Requirements

### Python 3.9+

```
flask>=3.0.0
waitress>=3.0.0
faster-whisper>=1.0.0
requests>=2.31.0
edge-tts>=6.1.0
miniaudio>=1.58
scipy>=1.13.0
numpy>=1.26.0
```

### Ollama

Download from [ollama.com](https://ollama.com), then:

```bash
ollama pull llama3.2:1b
```

### Arduino libraries (via Library Manager)

- `Adafruit SSD1306`
- `Adafruit GFX Library`
- ESP32 board support by Espressif (via Boards Manager URL)

---

## Server setup

### 1. Install Python dependencies

```bash
pip install -r requirements.txt
```

### 2. Start Ollama

```bash
ollama serve
```

### 3. Run the server

```bash
python app.py
```

Expected output:

```
==================================================
  ESP32-S3 × Ollama Voice Assistant
  STT  : faster-whisper tiny.en
  LLM  : llama3.2:1b via Ollama
  TTS  : edge-tts  en-US-JennyNeural
  IP   : 192.168.1.10:5000
  URL  : http://192.168.1.10:5000/chat
==================================================
Serving with Waitress (production)...
```

Copy the URL into `SERVER_URL` in the firmware.

---

## Firmware setup

### 1. Open `esp32_ollama/esp32_ollama.ino` in Arduino IDE

### 2. Edit the config block

```cpp
const char* WIFI_SSID     = "YOUR_SSID";
const char* WIFI_PASSWORD = "YOUR_PASSWORD";
const char* SERVER_URL    = "http://192.168.X.X:5000/chat";
```

### 3. Arduino IDE board settings

| Setting | Value |
|---|---|
| Board | ESP32S3 Dev Module |
| USB CDC On Boot | Enabled |
| PSRAM | OPI PSRAM |
| Upload Speed | 921600 |
| Flash Size | 8MB |

### 4. Upload, open Serial Monitor at 115200 baud

---

## Configuration

### Wake word (`app.py`)

```python
WAKE_WORD = "abi,bi,be"   # comma-separated list, case-insensitive
```

Set to `""` to disable the gate and respond to all speech.

### TTS voice (`app.py`)

```python
TTS_VOICE = "en-US-JennyNeural"
```

Popular options:

| Voice | Style |
|---|---|
| `en-US-JennyNeural` | Female, friendly (default) |
| `en-US-AriaNeural` | Female, natural |
| `en-US-GuyNeural` | Male, casual |
| `en-GB-SoniaNeural` | Female, British |
| `en-AU-NatashaNeural` | Female, Australian |

List all voices:

```bash
edge-tts --list-voices
```

### VAD sensitivity (`esp32_ollama.ino`)

```cpp
#define VAD_THRESHOLD      4000   // RMS loudness level — raise to reduce false triggers
#define VAD_WINDOW_SAMPLES  160   // 10 ms per window at 16 kHz
#define VAD_WINDOW_COUNT      8   // consecutive loud windows needed (80 ms total)
```

| Problem | Fix |
|---|---|
| Triggers on background noise | Increase `VAD_THRESHOLD` |
| Doesn't trigger on speech | Decrease `VAD_THRESHOLD` |
| Triggers on claps / bangs | Increase `VAD_WINDOW_COUNT` |

---

## Troubleshooting

| Problem | Fix |
|---|---|
| `Error -1` | PC and ESP32 on different networks |
| `Error -11` (timeout) | Ollama not running or model not pulled |
| `Error 500` | Check `python app.py` terminal for traceback |
| OLED blank | Check SDA=GPIO1, SCL=GPIO2, I2C address=`0x3C` |
| No audio | Check MAX98357A: BCLK=8, LRC=9, DIN=18 |
| Distorted audio | Verify `SAMPLE_RATE = 16000` in `app.py` |
| TTS not working | Check internet connection (edge-tts is online) |
| Whisper slow first run | Downloads `tiny.en` model (~75 MB) once and caches it |
| Keeps looping / echo trigger | Increase `VAD_THRESHOLD` or `VAD_WINDOW_COUNT` |

---

## Project structure

```
esp32-ollama/
├── app.py                  # Python backend — STT + LLM + TTS
├── requirements.txt        # Python dependencies with minimum versions
├── README.md
└── esp32_ollama/
    └── esp32_ollama.ino    # ESP32-S3 firmware
```

---

## Roadmap

- [x] INMP441 I2S microphone capture
- [x] MAX98357A I2S amplifier playback
- [x] SSD1306 OLED display with scrolling reply text
- [x] Status LEDs
- [x] Window-based RMS VAD (noise robust)
- [x] faster-whisper local STT
- [x] Ollama local LLM
- [x] edge-tts neural TTS with scipy polyphase resampling
- [x] Wake-word gate (comma-separated, case-insensitive)
- [x] FreeRTOS scroll task — OLED updates while audio plays
- [x] Startup jingle + wake-word confirmation beep
- [ ] Push-button manual trigger (bypass VAD in noisy environments)
- [ ] Conversation memory (multi-turn context in Ollama)
- [ ] Fully offline TTS (Piper TTS)
- [ ] Multi-turn mode (re-arm mic immediately after assistant asks a question)
