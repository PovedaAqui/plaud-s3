"""Edge cases for the ingest protocol, plus HTTP surface."""

import json
import time
import wave
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from app import main as server
from app.config import Settings


@pytest.fixture()
def client(tmp_path, monkeypatch):
    settings = Settings(data_dir=tmp_path, sync_pipeline=True)
    settings.ensure_dirs()
    monkeypatch.setattr(server, "settings", settings)
    def fake_process(**kw):
        meta = {
            "id": kw["recording_id"],
            "device": kw["device"],
            "duration": round(kw["duration"], 2),
        }
        (kw["wav_path"].parent / "meta.json").write_text(json.dumps(meta))
        return {"notes": "x"}

    monkeypatch.setattr(server, "process_recording", fake_process)
    return TestClient(server.app)


def _start(ws):
    ws.send_json({"type": "start", "device": "test"})
    assert ws.receive_json()["type"] == "started"


def test_pause_before_start_rejected(client):
    with client.websocket_connect("/ws/audio") as ws:
        ws.send_json({"type": "pause"})
        assert ws.receive_json() == {"type": "error", "error": "not recording"}


def test_stop_before_start_rejected(client):
    with client.websocket_connect("/ws/audio") as ws:
        ws.send_json({"type": "stop"})
        assert ws.receive_json()["type"] == "error"


def test_bytes_before_start_are_ignored(client, tmp_path):
    with client.websocket_connect("/ws/audio") as ws:
        ws.send_bytes(b"\x00\x00" * 4000)
        _start(ws)
        ws.send_bytes(b"\x00\x00" * 16000)
        ws.send_json({"type": "stop"})
        assert ws.receive_json()["type"] == "done"
    wavs = list(tmp_path.glob("*/audio.wav"))
    assert len(wavs) == 1
    with wave.open(str(wavs[0])) as f:
        assert f.getnframes() == 16000


def test_stop_while_paused_keeps_earlier_audio(client, tmp_path):
    with client.websocket_connect("/ws/audio") as ws:
        _start(ws)
        ws.send_bytes(b"\x00\x00" * 8000)
        ws.send_json({"type": "pause"})
        assert ws.receive_json()["type"] == "paused"
        ws.send_bytes(b"\x00\x00" * 8000)  # dropped
        ws.send_json({"type": "stop"})
        assert ws.receive_json()["type"] == "done"
    wav = next(tmp_path.glob("*/audio.wav"))
    with wave.open(str(wav)) as f:
        assert f.getnframes() == 8000


def test_disconnect_without_stop_saves_audio(client, tmp_path):
    with client.websocket_connect("/ws/audio") as ws:
        _start(ws)
        ws.send_bytes(b"\x00\x00" * 4000)
    wav = next(tmp_path.glob("*/audio.wav"), None)
    assert wav is not None
    with wave.open(str(wav)) as f:
        assert f.getnframes() == 4000


def test_two_sessions_do_not_mix(client, tmp_path):
    with client.websocket_connect("/ws/audio") as one, client.websocket_connect("/ws/audio") as two:
        _start(one)
        two.send_json({"type": "start", "device": "second"})
        assert two.receive_json()["type"] == "started"
        one.send_bytes(b"\x00\x00" * 4000)
        two.send_bytes(b"\x00\x00" * 8000)
        one.send_json({"type": "stop"})
        assert one.receive_json()["type"] == "done"
        two.send_json({"type": "stop"})
        assert two.receive_json()["type"] == "done"

    frames = sorted(
        wave.open(str(w)).getnframes() for w in tmp_path.glob("*/audio.wav")
    )
    assert frames == [4000, 8000]


def test_malformed_json_does_not_kill_session(client):
    with client.websocket_connect("/ws/audio") as ws:
        ws.send_text("not json")
        assert ws.receive_json()["type"] == "error"
        _start(ws)
        ws.send_json({"type": "stop"})
        assert ws.receive_json()["type"] == "done"


def test_unsupported_sample_rate_rejected(client):
    with client.websocket_connect("/ws/audio") as ws:
        ws.send_json({"type": "start", "device": "test", "rate": 8000})
        assert ws.receive_json() == {"type": "error", "error": "unsupported rate"}


def test_pipeline_crash_still_acks_client(tmp_path, monkeypatch):
    settings = Settings(data_dir=tmp_path, sync_pipeline=True)
    settings.ensure_dirs()
    monkeypatch.setattr(server, "settings", settings)

    def boom(**kw):
        raise RuntimeError("asr exploded")

    monkeypatch.setattr(server, "process_recording", boom)
    client = TestClient(server.app)
    with client.websocket_connect("/ws/audio") as ws:
        _start(ws)
        ws.send_bytes(b"\x00\x00" * 1600)
        ws.send_json({"type": "stop"})
        assert ws.receive_json()["type"] == "done"
    assert next(tmp_path.glob("*/audio.wav")).exists()


def test_health_and_listing(client, tmp_path):
    assert client.get("/health").json()["ok"] is True
    with client.websocket_connect("/ws/audio") as ws:
        _start(ws)
        ws.send_bytes(b"\x00\x00" * 1600)
        ws.send_json({"type": "stop"})
        ws.receive_json()
    listing = client.get("/recordings").json()
    assert len(listing) == 1
    assert listing[0]["device"] == "test"
    assert client.get(f"/recordings/{listing[0]['id']}").status_code == 200
    assert client.get("/recordings/nope").status_code == 404
