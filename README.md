# plaud-s3

A Plaud-Note-style voice recorder built on the **Waveshare ESP32-S3-Touch-AMOLED-1.75C**: tap the screen, talk, and the recording is transcribed and written into your Obsidian vault by a server you own.

This is not a Plaud-compatible client. It is the other half of the research report's "capture + ownership" gap: Plaud's own app is bypassed entirely.

```
ESP32-S3 (dual mic, 16 kHz)  --Wi-Fi/WebSocket-->  FastAPI ingest  -->  faster-whisper  -->  Markdown notes
      touch UI, timer              PCM s16le            WAV + metadata        transcript            vault/
```

## Hardware

| Property | Value |
|---|---|
| Board | Waveshare ESP32-S3-Touch-AMOLED-1.75C |
| MCU | ESP32-S3, 32 MB flash, 8 MB octal PSRAM |
| Display | 1.75" 466×466 AMOLED (CO5300), CST9217 touch |
| Audio | dual digital mics via ES7210 ADC, ES8311 codec, speaker |
| Sensors/power | QMI8658 IMU, AXP2101 PMIC |
| USB | native USB-Serial/JTAG (`303a:1001`), COM4 on this machine |

The factory firmware (Brookesia + Xiaozhi chat UI) is preserved in `backups/` before flashing.

## Repository layout

```
firmware/            ESP-IDF 5.5 application
  main/main.c        boot: display, UI, Wi-Fi, recorder task
  main/recorder.c    codec → downmix → WebSocket state machine
  main/ui.c          LVGL screen: status, timer, REC/STOP button
server/              Python ingest + transcription
  app/main.py        /ws/audio, /health, /recordings
  app/pipeline.py    faster-whisper, optional pyannote, LLM notes
  app/wavio.py       streaming WAV writer
  tools/send_wav.py  replay a WAV file through the same protocol
```

## Server

```bash
cd server
python3 -m venv .venv && .venv/bin/pip install -e .[dev]
PLAUD_DATA_DIR=./data PLAUD_VAULT_DIR="$HOME/Documents/Obsidian" \
  .venv/bin/uvicorn app.main:app --host 0.0.0.0 --port 8000
```

Configuration is environment-only (see `.env.example`):

| Variable | Default | Meaning |
|---|---|---|
| `PLAUD_DATA_DIR` | `./data` | audio, transcripts, notes per recording |
| `PLAUD_VAULT_DIR` | unset | if set, notes are also copied to `<dir>/Plaud/` |
| `PLAUD_ASR_MODEL` | `small` | faster-whisper model (`tiny`, `base`, `small`, `medium`, `large-v3`) |
| `PLAUD_ASR_DEVICE` / `PLAUD_ASR_COMPUTE` | `cpu` / `int8` | `cuda` + `float16` if you have a GPU |
| `PLAUD_LANGUAGE` | auto | force a language code |
| `PLAUD_DIARIZE` / `PLAUD_HF_TOKEN` | off | pyannote speaker labels |
| `PLAUD_LLM_BASE_URL` / `PLAUD_LLM_MODEL` | Ollama `127.0.0.1:11434/v1` / `llama3.2` | any OpenAI-compatible endpoint |
| `PLAUD_LLM_TIMEOUT` | 8 | seconds before falling back to "no summary" |

If no LLM answers, the notes file says so and contains the raw transcript — no invented summary.

### Protocol

1. device → text `{"type":"start","device":"plaud-s3-b0:10","rate":16000}`
2. server → text `{"type":"started","id":"20261103-165239"}`
3. device → binary frames of 16 kHz mono s16le (6400 bytes = 200 ms is fine)
4. device → text `{"type":"stop"}`
5. server closes the WAV, transcribes, writes `audio.wav`, `transcript.{txt,json}`, `notes.md`, `meta.json`, then replies `{"type":"done", ...}`

## Firmware

Credentials live in `scripts/wifi.env` (gitignored), not in menuconfig, so the build
can be reproduced without editing generated config:

```bash
cp scripts/wifi.env.example scripts/wifi.env
$EDITOR scripts/wifi.env        # SSID, password, ws:// URL of the server
./scripts/configure_wifi.sh     # regenerates sdkconfig, patches the BSP, builds
```

The USB device is currently attached to WSL as `/dev/ttyACM0`:

```bash
source ~/esp-idf/export.sh
cd firmware
idf.py -p /dev/ttyACM0 flash monitor
```

To flash from Windows instead, attach the device back with
`usbipd detach --busid 4-1` and write the three images individually — writing a
merged image at `0x0` would also overwrite the `nvs` partition at `0x9000`:

```powershell
py -3 -m esptool --chip esp32s3 -p COM4 -b 921600 write_flash `
  --flash_mode dio --flash_freq 80m --flash_size 32MB `
  0x0 bootloader.bin 0x8000 partition-table.bin 0x10000 plaud_s3.bin
```

### Behaviour

- Boots to a dark screen with a DiceBear avatar for the current state, a timer and a tap hint.
- Boot pre-connects the WebSocket so a tap starts capture in well under a second.
- Tap the avatar: opens the ES7210 at 16 kHz stereo, downmixes to mono, streams 100 ms chunks.
- The avatar and hint change per state; tap again to stop.
- If Wi-Fi or the server is down the screen shows the offline avatar and the mic is closed;
  nothing is buffered on flash yet.

| State | Avatar (DiceBear 10.x) | Hint |
|---|---|---|
| connecting | `rings` | WAIT |
| ready | `micah` | TAP TO RECORD |
| recording | `gaze` | TAP TO STOP |
| saving | `bottts` | WAIT |
| offline | `fun-emoji` | TAP TO RETRY |

Avatars are fetched once and baked into `firmware/main/avatars.c` as LVGL RGB565 images
(180×180, composited onto black). Regenerate after changing seeds or styles:

```bash
pip install pillow
python3 scripts/gen_avatars.py     # writes firmware/assets/avatars/*.png + avatars.c/h
```

## Verified so far

- Server: 4 tests cover WAV header correctness, WebSocket ingest, honest no-LLM notes, vault publishing.
- End-to-end: an 11 s speech sample streamed through the real protocol was transcribed by `tiny` as the JFK line, notes written and published to a vault directory.
- Firmware: builds with ESP-IDF 5.5.5, flashes and boots on the real board. Display, touch
  controller (CST9217) and PMIC initialise; the mic self-test reads 32,000 samples per second
  with a non-zero signal level (RMS 35-46, peak 280-414 in a quiet room).
- Wi-Fi: joins the 2.4 GHz AP (channel 11), gets `192.168.1.47`, and keeps a WebSocket open to
  the ingest server from boot (connected ~1.2 s after DHCP).
- End to end: autotest recordings from the board arrived at the server as `plaud-s3-cbb010`,
  with the firmware's frame counter matching the server's WAV duration exactly (e.g. 88,000
  samples = 5.5 s), zero dropped frames, and the note published to `Obsidian/Plaud/`.
- Not verified: the physical tap itself. The REC button's click handler is wired and logged,
  but no one has touched the panel; the same `recorder_toggle()` path is exercised by the
  autotest build.

### Display notes

The panel is a 466×466 QSPI AMOLED driven through `esp_lv_adapter` with partial refresh.
Two BSP adjustments are required and are re-applied by `scripts/patch_bsp_display.sh`
(the component manager overwrites them on a clean fetch):

- render bands are 20 rows instead of 50 — a 180 px repaint at 50 rows exceeds the DMA
  budget and returns `ESP_ERR_NO_MEM` from `esp_lcd_panel_draw_bitmap`;
- `max_transfer_sz` is capped at 50 rows so the SPI driver never reserves a full-screen
  transfer buffer.

Even with the stock 50-row bands, this panel showed stale bands after a large restyle
(half a circle keeping the previous colour). The UI therefore changes avatars and text
rather than restyling large widgets, and state changes repaint complete objects.

### Smoke-test hook

`CONFIG_PLAUD_AUTOTEST_SECONDS` (default 0) records automatically for N seconds after boot and
then stops. It exists so the whole path can be exercised over serial without touching the panel:

```bash
printf 'CONFIG_PLAUD_AUTOTEST_SECONDS=8\n' >> firmware/sdkconfig.local
rm firmware/sdkconfig && cd firmware && SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.local" idf.py build
```

Remember to remove the line and regenerate `sdkconfig` afterwards.

## Known limits (deliberate, for v0)

- **Streaming only.** No offline recording, no flash ring buffer. If Wi-Fi drops mid-recording the audio is lost.
- **16 kHz mono.** Enough for ASR; not a hi-fi recorder.
- **No authentication.** The WebSocket is open on the LAN. Do not expose it to the internet as-is.
- **Single concurrent pipeline per process** is fine for one device; `faster-whisper` is not parallelised across recordings.
- **No wake word, no VAD gating** on the device; recording starts on tap.
- **Speaker labels need a Hugging Face token** and add minutes per hour of audio on CPU.
