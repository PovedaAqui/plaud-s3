"""Edge cases for the ingest protocol, plus HTTP surface."""

import json
import wave

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


def _start(ws, **extra):
    ws.send_json({"type": "start", "device": "test", **extra})
    message = ws.receive_json()
    assert message["type"] == "started"
    return message


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

    frames = []
    for path in tmp_path.glob("*/audio.wav"):
        with wave.open(str(path)) as f:
            frames.append(f.getnframes())
    frames.sort()
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


def test_start_twice_rejected(client):
    with client.websocket_connect("/ws/audio") as ws:
        _start(ws)
        ws.send_json({"type": "start", "device": "again"})
        assert ws.receive_json() == {"type": "error", "error": "already recording"}


def test_rate_as_string_is_accepted(client):
    with client.websocket_connect("/ws/audio") as ws:
        ws.send_json({"type": "start", "device": "test", "rate": "16000"})
        assert ws.receive_json()["type"] == "started"
        ws.send_json({"type": "stop"})
        assert ws.receive_json()["type"] == "done"


def test_pause_is_idempotent_and_resume_without_pause_works(client):
    with client.websocket_connect("/ws/audio") as ws:
        _start(ws)
        ws.send_json({"type": "pause"})
        assert ws.receive_json()["type"] == "paused"
        ws.send_json({"type": "pause"})
        assert ws.receive_json()["type"] == "paused"
        ws.send_json({"type": "resume"})
        assert ws.receive_json()["type"] == "resumed"
        ws.send_json({"type": "resume"})
        assert ws.receive_json()["type"] == "resumed"
        ws.send_json({"type": "stop"})
        assert ws.receive_json()["type"] == "done"


def test_empty_recording_is_valid(client, tmp_path):
    with client.websocket_connect("/ws/audio") as ws:
        _start(ws)
        ws.send_json({"type": "stop"})
        done = ws.receive_json()
        assert done["type"] == "done" and done["duration"] == 0.0
    wav = next(tmp_path.glob("*/audio.wav"))
    with wave.open(str(wav)) as f:
        assert f.getnframes() == 0


def test_device_name_is_truncated(client, tmp_path):
    with client.websocket_connect("/ws/audio") as ws:
        ws.send_json({"type": "start", "device": "x" * 100})
        ws.receive_json()
        ws.send_json({"type": "stop"})
        ws.receive_json()
    meta = json.loads(next(tmp_path.glob("*/meta.json")).read_text())
    assert len(meta["device"]) == 64


def test_garbage_text_mid_recording_keeps_session(client, tmp_path):
    with client.websocket_connect("/ws/audio") as ws:
        _start(ws)
        ws.send_text("this is not json")
        assert ws.receive_json()["type"] == "error"
        ws.send_bytes(b"\x00\x00" * 1600)
        ws.send_json({"type": "stop"})
        assert ws.receive_json()["type"] == "done"
    wav = next(tmp_path.glob("*/audio.wav"))
    with wave.open(str(wav)) as f:
        assert f.getnframes() == 1600


def test_corrupt_meta_is_skipped(client, tmp_path):
    bad = tmp_path / "20260101-000000-dead"
    bad.mkdir()
    (bad / "meta.json").write_text("{not json")
    assert client.get("/recordings").json() == []


def test_disconnect_while_paused_finalises(client, tmp_path):
    with client.websocket_connect("/ws/audio") as ws:
        _start(ws)
        ws.send_bytes(b"\x00\x00" * 8000)
        ws.send_json({"type": "pause"})
        ws.receive_json()
    wav = next(tmp_path.glob("*/audio.wav"))
    with wave.open(str(wav)) as f:
        assert f.getnframes() == 8000


def test_session_ids_are_unique_within_one_second(client, tmp_path):
    ids = set()
    for _ in range(5):
        with client.websocket_connect("/ws/audio") as ws:
            ids.add(_start(ws)["id"])
            ws.send_json({"type": "stop"})
            ws.receive_json()
    assert len(ids) == 5
    assert len(list(tmp_path.glob("*/audio.wav"))) == 5
