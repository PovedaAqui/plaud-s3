# Changelog

All notable changes to this project. Dates are ISO-8601.

## [0.3.0] — 2026-11-04

### Added
- Pocket-watch screensaver: an always-on analog face (no seconds hand) after
  `CONFIG_PLAUD_SCREENSAVER_S` idle seconds, dimmed to `CONFIG_PLAUD_DIM_BRIGHTNESS`.
  The first tap or button press only wakes the device, so a brush against the glass cannot
  start a recording.
- NTP time sync (`CONFIG_PLAUD_NTP_SERVER`) plus a POSIX `CONFIG_PLAUD_TZ`; the face shows
  `SYNC` until time is known.
- Screen brightness is configurable (`CONFIG_PLAUD_BRIGHTNESS`).

## [0.2.3] — 2026-11-04

### Fixed
- An empty or malformed `PLAUD_LLM_BASE_URL` aborted the whole recording pipeline before the
  transcript was written; it now degrades to the "no summary" note like any unreachable model.

### Added
- `tools/e2e_check.py` / `make e2e`: black-box check that starts a server, streams speech and
  asserts the transcript, WAV and fallback note.

## [0.2.2] — 2026-11-04

### Fixed
- The device reconnects after the server closes the WebSocket cleanly. `esp_websocket_client`
  stops on a clean CLOSE unless `enable_close_reconnect` is set; without it an idle recorder
  stayed offline after a server restart and only came back on reboot.

## [0.2.1] — 2026-11-04

### Fixed
- The upload queue drains back-to-back instead of one file per retry interval, so a backlog
  after a long offline period clears in seconds.

## [0.2.0] — 2026-11-04

### Added
- **Offline-first recording**: audio is written to a 16 MB FAT partition first and uploaded
  by a retry loop (boot + every 5 s), so a missing server or reboot loses nothing.
- Optional microSD via SDMMC 1-bit; new recordings go to the card, pending internal files are
  drained first (Kconfig, off by default).
- Pause/resume: the paused stretch is dropped server-side, the timer freezes and no silence gap
  is written.
- DiceBear `gaze` avatars at 240 px, one per state, with colour/eye variants.
- Physical controls: Key1 on GPIO0 and Key2 on the AXP2101 `PWRON` interrupt status.
- Summary testing against `openbmb/MiniCPM5-1B`, including `PLAUD_LLM_EXTRA_BODY` for
  per-provider request switches (e.g. disable thinking).
- `CONTRIBUTING.md`, `SECURITY.md`, ruff linting, GitHub Actions for tests and firmware build,
  `tools/check_summary.py`, `tools/send_wav.py`.

### Changed
- Upload chunks raised from 4 KB to 16 KB (1 MB upload: 15.4 s → 3.1 s).
- Note generation split into two focused LLM calls (notes, then action items).
- Session ids include a random suffix; two devices can start in the same second.
- UI: text layers above the avatar; timer resets to `00:00` on stop.

### Fixed
- Hold-to-stop no longer starts a new recording on release.
- A broken vault path no longer aborts a finished recording.
- FAT long filenames (`rec_000001.wav`) rejected by the default 8.3 configuration.
- `ESP_ERR_NO_MEM` / stale bands when large widgets repaint (20-row render bands).

## [0.1.0] — 2026-11-03

- Initial public release: ESP32-S3 firmware, WebSocket ingest, faster-whisper transcription,
  optional diarization, LLM notes and Obsidian export.
