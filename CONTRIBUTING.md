# Contributing

Thanks for wanting to improve this. It is a small project: a firmware for one board and a
local server. Bug reports, hardware notes and focused pull requests are all welcome.

This project is independent and not affiliated with Plaud Inc.

## Before you start

- **Small fixes and docs**: open a PR directly.
- **Behaviour changes, new hardware or new dependencies**: open an issue first, so the design
  can be agreed before you spend time on it.
- **Security issues**: do not open an issue; see [SECURITY.md](SECURITY.md).

## Repository layout

```
firmware/            ESP-IDF application (C)
  main/              boot, recorder state machine, UI, buttons, storage, avatars
  assets/avatars/    DiceBear PNG sources
server/              FastAPI ingest + transcription pipeline (Python 3.11+)
  app/               config, protocol, WAV writer, ASR/LLM pipeline
  tests/             pytest suite
  tools/             send_wav.py, check_summary.py
scripts/             credential flow, BSP patch, avatar generator
docs/                README assets
```

## Server development

```bash
python3 -m venv .venv
.venv/bin/pip install -e "./server[dev]"

make test       # pytest
make lint       # ruff (E, F, I, UP; line length 100)
make server     # uvicorn on :8000 with the default environment
```

Coverage while developing:

```bash
cd server && PYTHONPATH=. ../.venv/bin/python -m pytest -q --cov=app --cov-report=term-missing
```

`make e2e` runs `server/tools/e2e_check.py`: it boots its own server on port 8099, streams a
short public-domain clip and checks the transcript, WAV and fallback note. Use it after touching
the ingest protocol or the pipeline.

Tests must not reach the network or a real model: monkeypatch `_llm_chat`, `urllib.request.urlopen`
or `_get_whisper` as the existing tests do. A test that needs an LLM endpoint belongs in
`tools/check_summary.py`, not in `pytest`.

## Firmware development

Requires ESP-IDF **5.5.x**.

```bash
source ~/esp-idf/export.sh
cp scripts/wifi.env.example scripts/wifi.env   # SSID, password, ws:// URL
./scripts/configure_wifi.sh                     # sdkconfig, BSP patch, build
make flash PORT=/dev/ttyACM0
```

Notes:

- `scripts/patch_bsp_display.sh` is **required**: the stock BSP renders 50-row bands that exceed
  the DMA budget for large widgets. `configure_wifi.sh` and `make firmware` apply it after the
  component manager fetches the BSP; re-run it if you delete `firmware/managed_components/`.
- Never commit `scripts/wifi.env`, `firmware/sdkconfig`, `firmware/sdkconfig.local` or the flash
  backup under `backups/`. They are gitignored; keep it that way.
- The avatar blobs in `firmware/main/assets/` are generated and committed. Only regenerate them
  when changing the look: `pip install pillow && python3 scripts/gen_avatars.py`.
- Keep dynamic allocation out of the audio path; the recorder runs on a fixed 100 ms chunk with
  static buffers.
- The display path is fragile: avoid large widget restyles. State changes swap the avatar and
  text instead of recolouring big objects.

### Hardware testing

`CONFIG_PLAUD_AUTOTEST_SECONDS=N` (0 = off) records, pauses, resumes and stops N/10 seconds per
phase after boot, so the radio, storage and upload path can be exercised from serial without
touching the panel. Please state which board revision and which tests you ran in the PR.

## Code style

- **Python**: ruff clean, 4-space indent, type hints on public functions, no hardcoded endpoints
  or credentials — everything tunable goes through `Settings` / environment variables.
- **C**: ESP-IDF conventions, `static` for file-local helpers, `ESP_LOG*` with a per-file tag,
  error paths that keep the audio safe (a failed upload must not lose a recording).
- **Docs**: update `README.md` when behaviour, configuration or the protocol changes.

## Pull requests

- Keep them focused; one concern per PR.
- CI must be green: `server` (ruff + pytest) and `firmware` (ESP-IDF build from scratch).
- Describe what you tested: host-only, device, which board/firmware, and anything not verified.
- Add a test when fixing a bug in the server. For firmware fixes, describe the serial evidence
  (or add an autotest variant) since the display and buttons cannot be asserted in CI.

## License

By contributing you agree that your changes are licensed under the MIT license in
[LICENSE](LICENSE).
