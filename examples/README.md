# Example output

`meeting/` is one complete run through the pipeline:

| File | Contents |
|---|---|
| `audio.wav` | 25.7 s, 16 kHz mono — a synthetic meeting recorded with Windows SAPI |
| `transcript.txt` / `.json` | faster-whisper `tiny` output with timestamps |
| `notes.md` | summary, action items and topics from MiniCPM5-1B, plus the transcript |
| `meta.json` | duration, language, segment count, title |

The voice is synthesized on purpose: the clip can live in the repository without shipping a
real person's conversation. Everything else is genuine model output — the note is not hand-edited,
and the 1B model's summary is imperfect (one decision is listed as an action item).

Reproduce it against a running server:

```bash
cd server
PLAUD_DATA_DIR=../examples/run \
PLAUD_ASR_MODEL=tiny \
PLAUD_LLM_BASE_URL=http://127.0.0.1:8080/v1 \
PLAUD_LLM_MODEL=minicpm5-1b \
PLAUD_LLM_MAX_TOKENS=400 \
PLAUD_LLM_EXTRA_BODY='{"chat_template_kwargs":{"enable_thinking":false},"frequency_penalty":0.5,"presence_penalty":0.5}' \
  python tools/send_wav.py ../examples/meeting/audio.wav
```

The run lands in `examples/run/<recording-id>/`; delete it afterwards. `make e2e` performs the
same check automatically on a temporary server.
