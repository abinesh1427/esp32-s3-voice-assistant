#include <WiFi.h>
#include <HTTPClient.h>
#include <driver/i2s.h>
#include "esp_heap_caps.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

// ─────────────────────────────────────────────
//  CONFIGURATION  (edit these!)
// ─────────────────────────────────────────────
const char* WIFI_SSID     = "HOME";
const char* WIFI_PASSWORD = "$home*12345";
const char* SERVER_URL    = "http://192.168.7.6:5000/chat";

// ─────────────────────────────────────────────
//  PIN DEFINITIONS  (do not change)
// ─────────────────────────────────────────────
#define OLED_SDA      1
#define OLED_SCL      2
#define LED_RED       10
#define LED_GREEN     11

// INMP441 microphone (I2S_NUM_0)
#define I2S_MIC_WS   15
#define I2S_MIC_SCK  16
#define I2S_MIC_SD   17

// MAX98357A amplifier (I2S_NUM_1)
#define I2S_SPK_LRC   9
#define I2S_SPK_BCLK  8
#define I2S_SPK_DIN  18

// ─────────────────────────────────────────────
//  AUDIO SETTINGS
// ─────────────────────────────────────────────
#define SAMPLE_RATE      16000
#define RECORD_SECS      4
#define BYTES_PER_SAMPLE 2

// VAD — window-based energy detector (robust against transient background noise)
// Computes RMS over a short window; requires several consecutive loud windows before triggering.
// Increased to ignore ambient background noise and accidental short human sounds (coughs, grunts).
#define VAD_THRESHOLD      4000   // RMS level to consider a window "loud"
#define VAD_WINDOW_SAMPLES  160   // samples per window (10 ms at 16 kHz)
#define VAD_WINDOW_COUNT      8   // consecutive loud windows needed (= 80 ms of sustained human speech)

// Buffer allocated from PSRAM to avoid DRAM overflow (~128KB)
#define AUDIO_BUF_SIZE (SAMPLE_RATE * BYTES_PER_SAMPLE * RECORD_SECS + 44)
static uint8_t* audioBuf = nullptr;

// ─────────────────────────────────────────────
//  OLED
// ─────────────────────────────────────────────
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
static SemaphoreHandle_t oledMutex = NULL;

void oled(const char* line1, const char* line2 = "") {
  // Guard against being called before mutex is created (early setup)
  if (oledMutex && xSemaphoreTake(oledMutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    Serial.println(line1);
    return;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setTextSize(2);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, 10);
  display.print(line1);

  if (strlen(line2) > 0) {
    display.setTextSize(1);
    display.getTextBounds(line2, 0, 0, &x1, &y1, &w, &h);
    display.setCursor((SCREEN_WIDTH - w) / 2, 44);
    display.print(line2);
  }
  display.display();

  if (oledMutex) xSemaphoreGive(oledMutex);
  Serial.println(line1);
}

// ─────────────────────────────────────────────
//  OLED SCROLL TASK  (runs on Core 0)
// ─────────────────────────────────────────────
#define SCROLL_MAX_LINES  16
#define CHARS_PER_LINE    21
#define VISIBLE_LINES      4
#define LINE_Y_START      18
#define LINE_HEIGHT       10
#define SCROLL_INTERVAL_MS 2200   // ms per line advance


static volatile bool      scrollActive = false;
static String             scrollLines[SCROLL_MAX_LINES];
static uint8_t            scrollLineCount = 0;

// Word-wrap text into scrollLines[], returns line count
static uint8_t buildScrollLines(const String& text) {
  uint8_t count = 0;
  String rem = text;
  while (rem.length() > 0 && count < SCROLL_MAX_LINES) {
    if ((int)rem.length() <= CHARS_PER_LINE) {
      scrollLines[count++] = rem;
      rem = "";
    } else {
      int cut = rem.lastIndexOf(' ', CHARS_PER_LINE);
      if (cut <= 0) cut = CHARS_PER_LINE;
      scrollLines[count++] = rem.substring(0, cut);
      rem = rem.substring(cut + 1);
    }
  }
  return count;
}

// FreeRTOS task — scrolls OLED on Core 0 while Core 1 plays audio
void oledScrollTask(void* pv) {
  uint8_t offset = 0;
  unsigned long lastTick = millis();

  while (true) {
    if (scrollActive) {
      if (xSemaphoreTake(oledMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);

        display.setTextSize(2);
        display.setCursor(16, 0);
        display.print("Speaking");

        display.setTextSize(1);
        for (uint8_t i = 0; i < VISIBLE_LINES; i++) {
          uint8_t idx = offset + i;
          if (idx < scrollLineCount) {
            display.setCursor(0, LINE_Y_START + i * LINE_HEIGHT);
            display.print(scrollLines[idx]);
          }
        }
        display.display();
        xSemaphoreGive(oledMutex);
      }

      // Advance one line at a time; loop back to top when end is reached
      if (scrollLineCount > VISIBLE_LINES &&
          millis() - lastTick >= SCROLL_INTERVAL_MS) {
        offset++;
        if (offset + VISIBLE_LINES > scrollLineCount) offset = 0;
        lastTick = millis();
      }
    } else {
      offset   = 0;
      lastTick = millis();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// Call before playResponse() — loads text and starts the scroll task rendering
void oledSpeaking(const String& text) {
  scrollLineCount = buildScrollLines(text);
  scrollActive    = true;
  Serial.println("Speaking");
}

// Call after playResponse() — stops scroll task, releases OLED back to main loop
void oledSpeakingStop() {
  scrollActive = false;
  vTaskDelay(pdMS_TO_TICKS(60));   // let the task finish its current frame
}

// ─────────────────────────────────────────────
//  WAV HEADER  (fills first 44 bytes of buf)
// ─────────────────────────────────────────────
void writeWAVHeader(uint8_t* buf, uint32_t dataBytes) {
  uint32_t fileSize = dataBytes + 36;
  uint32_t byteRate = SAMPLE_RATE * BYTES_PER_SAMPLE;
  buf[0]='R'; buf[1]='I'; buf[2]='F'; buf[3]='F';
  buf[4]=fileSize&0xFF; buf[5]=(fileSize>>8)&0xFF; buf[6]=(fileSize>>16)&0xFF; buf[7]=(fileSize>>24)&0xFF;
  buf[8]='W'; buf[9]='A'; buf[10]='V'; buf[11]='E';
  buf[12]='f'; buf[13]='m'; buf[14]='t'; buf[15]=' ';
  buf[16]=16; buf[17]=0; buf[18]=0; buf[19]=0;
  buf[20]=1; buf[21]=0;
  buf[22]=1; buf[23]=0;
  buf[24]=SAMPLE_RATE&0xFF; buf[25]=(SAMPLE_RATE>>8)&0xFF; buf[26]=(SAMPLE_RATE>>16)&0xFF; buf[27]=(SAMPLE_RATE>>24)&0xFF;
  buf[28]=byteRate&0xFF; buf[29]=(byteRate>>8)&0xFF; buf[30]=(byteRate>>16)&0xFF; buf[31]=(byteRate>>24)&0xFF;
  buf[32]=2; buf[33]=0;
  buf[34]=16; buf[35]=0;
  buf[36]='d'; buf[37]='a'; buf[38]='t'; buf[39]='a';
  buf[40]=dataBytes&0xFF; buf[41]=(dataBytes>>8)&0xFF; buf[42]=(dataBytes>>16)&0xFF; buf[43]=(dataBytes>>24)&0xFF;
}

// ─────────────────────────────────────────────
//  I2S SETUP
// ─────────────────────────────────────────────
void setupI2S() {
  // Microphone (I2S_NUM_0)
  i2s_config_t mic_cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 8,
    .dma_buf_len          = 512,
    .use_apll             = false,
    .tx_desc_auto_clear   = false,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t mic_pins = {
    .bck_io_num   = I2S_MIC_SCK,
    .ws_io_num    = I2S_MIC_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num  = I2S_MIC_SD
  };
  i2s_driver_install(I2S_NUM_0, &mic_cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &mic_pins);

  // Amplifier (I2S_NUM_1)
  i2s_config_t spk_cfg = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_MSB, // Better for MAX98357A (fixes staticky ringing)
    .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count        = 16,     // Increased to prevent buffer underflows over Wi-Fi
    .dma_buf_len          = 1024,   // 16KB total buffering
    .use_apll             = false,
    .tx_desc_auto_clear   = true,
    .fixed_mclk           = 0
  };
  i2s_pin_config_t spk_pins = {
    .bck_io_num   = I2S_SPK_BCLK,
    .ws_io_num    = I2S_SPK_LRC,
    .data_out_num = I2S_SPK_DIN,
    .data_in_num  = I2S_PIN_NO_CHANGE
  };
  i2s_driver_install(I2S_NUM_1, &spk_cfg, 0, NULL);
  i2s_set_pin(I2S_NUM_1, &spk_pins);
}

// ─────────────────────────────────────────────
//  TONE GENERATOR  (sine wave via I2S speaker)
// ─────────────────────────────────────────────
void playTone(uint16_t freqHz, uint16_t durationMs, float amplitude = 0.25f) {
  uint32_t totalSamples = (uint32_t)(SAMPLE_RATE * durationMs / 1000);
  uint16_t buf[128];
  uint32_t played = 0;
  float phase = 0.0f;
  float phaseStep = 2.0f * M_PI * freqHz / SAMPLE_RATE;

  while (played < totalSamples) {
    uint16_t chunk = min((uint32_t)128, totalSamples - played);
    for (uint16_t i = 0; i < chunk; i++) {
      // Fade in/out over first and last 10 ms to avoid clicks
      float env = 1.0f;
      uint32_t fadeSamples = SAMPLE_RATE / 100;   // 10 ms
      if (played + i < fadeSamples)
        env = (float)(played + i) / fadeSamples;
      else if (played + i > totalSamples - fadeSamples)
        env = (float)(totalSamples - played - i) / fadeSamples;

      buf[i] = (int16_t)(sinf(phase) * amplitude * 32767.0f * env);
      phase += phaseStep;
    }
    size_t bw;
    i2s_write(I2S_NUM_1, buf, chunk * 2, &bw, 100 / portTICK_PERIOD_MS);
    played += chunk;
  }
  i2s_zero_dma_buffer(I2S_NUM_1);
}

// Three-note ascending startup jingle
void playStartupJingle() {
  playTone(523, 100);   // C5
  delay(30);
  playTone(659, 100);   // E5
  delay(30);
  playTone(784, 180);   // G5  (held longer)
}

// Short double-beep: played when wake word is detected (before Speaking)
void playWakeBeep() {
  playTone(880, 60);    // A5
  delay(40);
  playTone(1047, 80);   // C6
}

// ─────────────────────────────────────────────
//  SETUP
// ─────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  pinMode(LED_RED,   OUTPUT); digitalWrite(LED_RED,   LOW);
  pinMode(LED_GREEN, OUTPUT); digitalWrite(LED_GREEN, LOW);

  oledMutex = xSemaphoreCreateMutex();

  Wire.begin(OLED_SDA, OLED_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
    Serial.println("OLED init failed");

  // Start OLED scroll task on Core 0 (main audio loop runs on Core 1)
  xTaskCreatePinnedToCore(oledScrollTask, "oled_scroll", 4096, NULL, 1, NULL, 0);

  oled("Starting..");

  // Allocate audio buffer from PSRAM (avoids DRAM overflow)
  audioBuf = (uint8_t*)ps_malloc(AUDIO_BUF_SIZE);
  if (!audioBuf) audioBuf = (uint8_t*)malloc(AUDIO_BUF_SIZE);
  if (!audioBuf) {
    oled("MEM ERROR");
    Serial.println("FATAL: cannot allocate audio buffer");
    while (1) delay(1000);
  }
  Serial.printf("Audio buffer: %d bytes OK\n", AUDIO_BUF_SIZE);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  oled("Connecting", WIFI_SSID);
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.printf("\nConnected! IP: %s\n", WiFi.localIP().toString().c_str());
  oled("Connected!", WiFi.localIP().toString().c_str());
  delay(2000);

  setupI2S();
  digitalWrite(LED_GREEN, HIGH);
  playStartupJingle();
}

// ─────────────────────────────────────────────
//  PLAY  WAV stream from HTTP response
// ─────────────────────────────────────────────
void playResponse(HTTPClient& http) {
  int len = http.getSize();
  WiFiClient* stream = http.getStreamPtr();
  uint8_t chunk[512];

  // Skip 44-byte WAV header
  int skip = 44;
  unsigned long t0 = millis();
  while (skip > 0 && millis() - t0 < 5000) {
    int avail = stream->available();
    if (avail > 0) {
      int r = stream->readBytes(chunk, min(avail, skip));
      skip -= r;
    } else delay(1);
  }

  // Stream PCM to amplifier with a timeout guard
  unsigned long lastData = millis();
  while (http.connected() && (len > 0 || len == -1)) {
    int avail = stream->available();
    if (avail > 0) {
      int r = stream->readBytes(chunk, min((int)sizeof(chunk), avail));
      size_t bw;
      i2s_write(I2S_NUM_1, chunk, r, &bw, 100 / portTICK_PERIOD_MS);
      if (len > 0) len -= r;
      lastData = millis();
    } else {
      if (millis() - lastData > 800) break;  // 800ms silence = end of audio
      delay(1);
    }
  }

  // Drain DMA so next playback starts clean and watchdog doesn't reset
  i2s_zero_dma_buffer(I2S_NUM_1);
  delay(300);  // let speaker physically stop vibrating before re-arming mic
}

// ─────────────────────────────────────────────
//  MAIN LOOP
// ─────────────────────────────────────────────
void loop() {
  // ── IDLE: wait for voice (VAD) ─────────────────────────────────────────
  oled("Listening", "speak to start");
  digitalWrite(LED_GREEN, HIGH);
  digitalWrite(LED_RED,   LOW);

  // Flush stale DMA data (800 reads * 4 bytes = 3200 bytes, clears full DMA pipeline)
  int32_t flush; size_t n;
  for (int i = 0; i < 800; i++)
    i2s_read(I2S_NUM_0, &flush, sizeof(flush), &n, portMAX_DELAY);

  // Window-based VAD: compute RMS over each 10 ms window.
  // Requires VAD_WINDOW_COUNT consecutive loud windows (50 ms total) to confirm real speech.
  // A door knock or clap is short; voice is sustained — this separates them reliably.
  int loudWindows = 0;
  while (true) {
    int64_t energy = 0;
    for (int i = 0; i < VAD_WINDOW_SAMPLES; i++) {
      int32_t s;
      i2s_read(I2S_NUM_0, &s, sizeof(s), &n, portMAX_DELAY);
      if (n > 0) {
        int16_t val = (int16_t)(s >> 11);
        energy += (int32_t)val * val;
      }
    }
    int16_t rms = (int16_t)sqrt((float)energy / VAD_WINDOW_SAMPLES);
    if (rms > VAD_THRESHOLD) {
      if (++loudWindows >= VAD_WINDOW_COUNT) break;   // 50 ms of sustained voice → record
    } else {
      loudWindows = 0;   // reset — windows must be consecutive
    }
  }

  // ── RECORDING ──────────────────────────────────────────────────────────
  oled("Recording", "...");
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_RED,   HIGH);

  uint32_t dataBytes = (uint32_t)(SAMPLE_RATE * BYTES_PER_SAMPLE * RECORD_SECS);
  writeWAVHeader(audioBuf, dataBytes);

  // Flush the DMA buffer again after VAD trigger so we don't capture stale data
  for (int i = 0; i < 50; i++)
    i2s_read(I2S_NUM_0, &flush, sizeof(flush), &n, portMAX_DELAY);

  int32_t tmp[128];
  uint8_t* p = audioBuf + 44;
  uint32_t written = 0;
  while (written < dataBytes) {
    size_t bytesIn = 0;
    i2s_read(I2S_NUM_0, tmp, sizeof(tmp), &bytesIn, portMAX_DELAY);
    int samples = bytesIn / 4;
    for (int i = 0; i < samples && written < dataBytes; i++) {
      int16_t val = (int16_t)(tmp[i] >> 11);
      *p++ = val & 0xFF;
      *p++ = (val >> 8) & 0xFF;
      written += 2;
    }
  }

  // ── SEND TO SERVER ─────────────────────────────────────────────────────
  oled("Thinking", "please wait...");

  HTTPClient http;
  http.setTimeout(60000);
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "audio/wav");
  // Collect X-Reply-Text header so we can display it on the OLED
  const char* hdrs[] = { "X-Reply-Text" };
  http.collectHeaders(hdrs, 1);
  int code = http.POST(audioBuf, dataBytes + 44);

  if (code == 200) {
    String replyText = http.header("X-Reply-Text");
    playWakeBeep();
    oledSpeaking(replyText);
    playResponse(http);
    oledSpeakingStop();
    delay(500);   // let speaker fully drain before looping
  } else if (code == 204) {
    // No wake word — go back to listening immediately, no sound played
    Serial.println("[GATE] No wake word, resuming listening");
  } else {
    Serial.printf("[ERROR] %d: %s\n", code, http.errorToString(code).c_str());
    oled("Error", http.errorToString(code).c_str());
    delay(2000);
  }
  http.end();

  // Cool-down only after a real response to prevent speaker echo retriggering
  if (code == 200) delay(2500);
}
