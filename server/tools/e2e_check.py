#!/usr/bin/env python3
"""End-to-end check: start the server, stream speech, verify the transcript.

    python tools/e2e_check.py

Downloads a short public-domain speech clip on first run, boots the ingest server
on a spare port with a temporary data directory and the `tiny` model, streams the
clip through the real WebSocket protocol, then asserts the transcript and notes
exist. Exit code 0 means the full pipeline works on this machine.
"""

from __future__ import annotations

import asyncio
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time
import urllib.request

import websockets

SERVER_DIR = pathlib.Path(__file__).resolve().parent.parent
SAMPLE_URL = "https://github.com/ggml-org/whisper.cpp/raw/master/samples/jfk.wav"
PORT = 8099
PHRASE = "ask not"


def ensure_sample(path: pathlib.Path) -> pathlib.Path:
    if not path.exists():
        print(f"downloading {SAMPLE_URL}")
        urllib.request.urlretrieve(SAMPLE_URL, path)
    return path


def wait_health(timeout: float = 30.0) -> None:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(f"http://127.0.0.1:{PORT}/health", timeout=1) as response:
                if response.status == 200:
                    return
        except OSError:
            time.sleep(0.25)
    raise SystemExit("server did not become healthy")


async def stream(wav: pathlib.Path) -> str:
    import wave

    with wave.open(str(wav)) as handle:
        pcm = handle.readframes(handle.getnframes())
        rate = handle.getframerate()
    async with websockets.connect(f"ws://127.0.0.1:{PORT}/ws/audio", max_size=None) as ws:
        await ws.send(json.dumps({"type": "start", "device": "e2e", "rate": rate}))
        started = json.loads(await ws.recv())
        assert started["type"] == "started", started
        chunk = rate * 2 // 10
        for offset in range(0, len(pcm), chunk):
            await ws.send(pcm[offset : offset + chunk])
        await ws.send(json.dumps({"type": "stop"}))
        done = json.loads(await ws.recv())
        assert done["type"] == "done", done
        return started["id"]


def main() -> int:
    sample = ensure_sample(pathlib.Path(tempfile.gettempdir()) / "e2e_jfk.wav")
    with tempfile.TemporaryDirectory() as data_dir:
        env = dict(os.environ)
        env.update(
            PLAUD_DATA_DIR=data_dir,
            PLAUD_ASR_MODEL=os.environ.get("PLAUD_ASR_MODEL", "tiny"),
            PLAUD_SYNC_PIPELINE="1",
            PLAUD_LLM_BASE_URL="",  # force the documented "no summary" fallback
        )
        server = subprocess.Popen(
            [sys.executable, "-m", "uvicorn", "app.main:app", "--port", str(PORT)],
            cwd=SERVER_DIR,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            wait_health()
            recording_id = asyncio.run(stream(sample))
            out = pathlib.Path(data_dir) / recording_id
            transcript = (out / "transcript.txt").read_text()
            notes = (out / "notes.md").read_text()
            print(transcript.strip())
            assert PHRASE in transcript.lower(), "transcript does not contain the speech"
            assert "## Transcript" in notes
            assert "No language model was reachable" in notes  # no LLM configured here
            assert (out / "audio.wav").stat().st_size > 100_000
            print("\nPASS: ingest -> whisper -> notes")
            return 0
        finally:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
            if server.returncode not in (0, -15):
                print(server.stdout.read() if server.stdout else "")


if __name__ == "__main__":
    raise SystemExit(main())
