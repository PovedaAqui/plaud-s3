import wave
from pathlib import Path

import pytest

from app import main as server
from app.config import Settings
from app.pipeline import Segment, build_notes, process_recording
from app.wavio import WavWriter


@pytest.fixture()
def data_dir(tmp_path, monkeypatch):
    test_settings = Settings(data_dir=tmp_path, sync_pipeline=True)
    test_settings.ensure_dirs()
    monkeypatch.setattr(server, "settings", test_settings)
    return tmp_path


def _recv(ws):
    """Receive the next protocol message, skipping the clock greeting."""
    message = ws.receive_json()
    while message.get("type") == "time":
        message = ws.receive_json()
    return message


def test_wav_writer_header_is_valid(tmp_path):
    path = tmp_path / "a.wav"
    with WavWriter(path, rate=16000) as w:
        w.write(b"\x00\x01" * 16000)
    with wave.open(str(path)) as f:
        assert f.getframerate() == 16000
        assert f.getnchannels() == 1
        assert f.getsampwidth() == 2
        assert f.getnframes() == 16000


def test_ws_ingest_writes_audio_and_finalises(data_dir, monkeypatch):
    calls = {}

    def fake_process(**kwargs):
        calls.update(kwargs)
        return {"notes": str(kwargs["wav_path"].parent / "notes.md")}

    monkeypatch.setattr(server, "process_recording", fake_process)

    from fastapi.testclient import TestClient

    client = TestClient(server.app)
    with client.websocket_connect("/ws/audio") as ws:
        ws.send_json({"type": "start", "device": "test-device"})
        started = _recv(ws)
        assert started["type"] == "started"
        ws.send_bytes(b"\x00\x00" * 16000)  # one second of silence
        ws.send_json({"type": "stop"})
        done = _recv(ws)
        assert done["type"] == "done"
        assert done["duration"] == pytest.approx(1.0, abs=0.01)

    wav_path = Path(calls["wav_path"])
    assert wav_path.exists()
    with wave.open(str(wav_path)) as f:
        assert f.getnframes() == 16000
    assert calls["device"] == "test-device"


def test_pause_drops_audio_between_resume(data_dir, monkeypatch):
    calls = {}

    def fake_process(**kwargs):
        calls.update(kwargs)
        return {"notes": "x"}

    monkeypatch.setattr(server, "process_recording", fake_process)

    from fastapi.testclient import TestClient

    client = TestClient(server.app)
    with client.websocket_connect("/ws/audio") as ws:
        ws.send_json({"type": "start", "device": "test"})
        assert _recv(ws)["type"] == "started"
        ws.send_bytes(b"\x00\x00" * 16000)          # 1 s recorded
        ws.send_json({"type": "pause"})
        assert _recv(ws)["type"] == "paused"
        ws.send_bytes(b"\x01\x01" * 16000)          # must be discarded
        ws.send_json({"type": "resume"})
        assert _recv(ws)["type"] == "resumed"
        ws.send_bytes(b"\x00\x00" * 16000)          # 1 s recorded
        ws.send_json({"type": "stop"})
        assert _recv(ws)["type"] == "done"

    with wave.open(str(calls["wav_path"])) as f:
        assert f.getnframes() == 32000  # 2 s, pause excluded


def test_notes_are_honest_without_llm(tmp_path, monkeypatch):
    settings = Settings(llm_base_url="http://127.0.0.1:9/v1", llm_timeout=0.2)
    title, md = build_notes(
        recording_id="20261103-161500",
        segments=[Segment(0.0, 2.0, "We ship on Friday.")],
        language="en",
        duration=12.0,
        device="esp32",
        settings=settings,
    )
    assert title == "Recording 20261103-161500"
    assert "No language model was reachable" in md
    assert "[00:00] Speaker 1: We ship on Friday." in md
    assert "## Action items" not in md


def test_process_recording_publishes_to_vault(tmp_path, monkeypatch):
    settings = Settings(
        data_dir=tmp_path / "data",
        vault_dir=tmp_path / "vault",
        llm_base_url="http://127.0.0.1:9/v1",
        llm_timeout=0.2,
    )
    settings.ensure_dirs()
    wav = settings.data_dir / "20261103-120000" / "audio.wav"
    wav.parent.mkdir()
    with WavWriter(wav) as w:
        w.write(b"\x00\x00" * 8000)

    monkeypatch.setattr(
        "app.pipeline.transcribe", lambda path, s: ([Segment(0, 1, "hello world")], "en")
    )
    meta = process_recording(
        recording_id="20261103-120000",
        wav_path=wav,
        device="esp32",
        duration=0.5,
        settings=settings,
    )
    published = Path(meta["published"])
    assert published.exists()
    assert published.read_text().startswith("# Recording 20261103-120000")
