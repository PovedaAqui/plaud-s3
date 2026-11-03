"""Unit tests for transcription helpers, diarization and LLM note generation."""

import io
import json
import wave
from pathlib import Path
from types import SimpleNamespace

import pytest

from app import pipeline
from app.config import Settings
from app.pipeline import (
    Segment,
    _llm_chat,
    _llm_summary,
    build_notes,
    diarize,
    format_transcript,
    process_recording,
    read_pcm,
    transcribe,
)
from app.wavio import WavWriter


def make_wav(path: Path, seconds: float = 0.5, rate: int = 16000) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    with WavWriter(path, rate=rate) as w:
        w.write(b"\x00\x00" * int(rate * seconds))
    return path


def fake_urlopen(payload: dict | str, status: int = 200):
    body = payload.encode() if isinstance(payload, str) else json.dumps(payload).encode()

    class Response(io.BytesIO):
        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return False

    def opener(request, timeout=None):
        if status >= 400:
            raise pipeline.urllib.error.HTTPError(request.full_url, status, "err", {}, None)
        return Response(body)

    return opener


def chat_payload(content: str) -> dict:
    return {"choices": [{"message": {"content": content}}]}


# ------------------------------------------------------------------ wav / pcm


def test_read_pcm_roundtrip(tmp_path):
    path = make_wav(tmp_path / "a.wav", 0.25)
    samples = read_pcm(path)
    assert len(samples) == 4000
    assert float(samples.max()) == 0.0


def test_read_pcm_rejects_stereo(tmp_path):
    path = tmp_path / "stereo.wav"
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(b"\x00\x00" * 200)
    with pytest.raises(ValueError, match="mono"):
        read_pcm(path)


def test_read_pcm_rejects_wrong_rate(tmp_path):
    path = make_wav(tmp_path / "8k.wav", 0.1, rate=8000)
    with pytest.raises(ValueError, match="16 kHz"):
        read_pcm(path)


def test_wav_writer_close_is_idempotent(tmp_path):
    path = tmp_path / "a.wav"
    writer = WavWriter(path)
    writer.write(b"\x00\x00" * 10)
    writer.close()
    writer.close()  # must not raise
    with wave.open(str(path)) as w:
        assert w.getnframes() == 10


# ------------------------------------------------------------------- ASR


def test_transcribe_maps_segments(tmp_path, monkeypatch):
    class Model:
        def transcribe(self, samples, **kwargs):
            assert kwargs["beam_size"] == 3
            assert kwargs["vad_filter"] is False
            seg = SimpleNamespace(start=0.5, end=1.5, text=" hello ")
            return iter([seg]), SimpleNamespace(language="en")

    monkeypatch.setattr(pipeline, "_get_whisper", lambda settings: Model())
    settings = Settings(asr_beam_size=3, asr_vad=False)
    segments, language = transcribe(make_wav(tmp_path / "a.wav"), settings)
    assert language == "en"
    assert segments == [Segment(0.5, 1.5, "hello")]


# ------------------------------------------------------------- diarization


class FakeTurn:
    def __init__(self, start, end):
        self.start = start
        self.end = end


class FakeAnnotation:
    def itertracks(self, yield_label=False):
        assert yield_label
        yield FakeTurn(0.0, 1.0), None, "SPEAKER_00"
        yield FakeTurn(1.0, 2.0), None, "SPEAKER_01"


def test_diarize_assigns_speakers_by_overlap(tmp_path, monkeypatch):
    monkeypatch.setattr(pipeline, "_load_diarizer", lambda settings: (lambda path: FakeAnnotation()))
    segments = [Segment(0.1, 0.9, "a"), Segment(1.2, 1.8, "b")]
    out = diarize(tmp_path / "a.wav", segments, Settings(diarize=True, hf_token="x"))
    assert [s.speaker for s in out] == ["SPEAKER_00", "SPEAKER_01"]


def test_diarize_degrades_when_model_missing(tmp_path, monkeypatch):
    def boom(settings):
        raise RuntimeError("no token")

    monkeypatch.setattr(pipeline, "_load_diarizer", boom)
    segments = [Segment(0, 1, "a")]
    out = diarize(tmp_path / "a.wav", segments, Settings(diarize=True))
    assert out[0].speaker == "Speaker 1"


# --------------------------------------------------------------- LLM calls


def test_llm_chat_parses_json_in_prose(monkeypatch):
    monkeypatch.setattr(
        pipeline.urllib.request, "urlopen", fake_urlopen(chat_payload('Sure:\n{"summary": "ok"}\n')))
    assert _llm_chat("prompt", Settings())["summary"] == "ok"


def test_llm_chat_rejects_non_json(monkeypatch):
    monkeypatch.setattr(pipeline.urllib.request, "urlopen", fake_urlopen(chat_payload("no json here")))
    assert _llm_chat("prompt", Settings()) is None


def test_llm_chat_returns_none_on_http_error(monkeypatch):
    monkeypatch.setattr(pipeline.urllib.request, "urlopen", fake_urlopen("{}", status=500))
    assert _llm_chat("prompt", Settings()) is None


def test_llm_chat_sends_temperature_and_max_tokens(monkeypatch):
    seen = {}

    def opener(request, timeout=None):
        seen.update(json.loads(request.data))
        body = json.dumps(chat_payload('{"summary": "x"}')).encode()

        class R(io.BytesIO):
            def __enter__(self):
                return self

            def __exit__(self, *exc):
                return False

        return R(body)

    monkeypatch.setattr(pipeline.urllib.request, "urlopen", opener)
    _llm_chat("prompt", Settings(llm_temperature=0.7, llm_max_tokens=64))
    assert seen["temperature"] == 0.7
    assert seen["max_tokens"] == 64


def test_llm_summary_merges_actions(monkeypatch):
    replies = iter(
        [
            {"title": "T", "summary": "S", "topics": ["a"]},
            {"action_items": ["do x", " ", "do y"]},
        ]
    )
    monkeypatch.setattr(pipeline, "_llm_chat", lambda prompt, settings: next(replies))
    notes = _llm_summary("transcript", Settings())
    assert notes["action_items"] == ["do x", "do y"]


def test_llm_summary_keeps_summary_when_actions_fail(monkeypatch):
    replies = iter([{"title": "T", "summary": "S"}, None])
    monkeypatch.setattr(pipeline, "_llm_chat", lambda prompt, settings: next(replies))
    notes = _llm_summary("transcript", Settings())
    assert notes["summary"] == "S"
    assert notes["action_items"] == []


def test_llm_summary_requires_summary(monkeypatch):
    monkeypatch.setattr(pipeline, "_llm_chat", lambda prompt, settings: {"title": "T"})
    assert _llm_summary("transcript", Settings()) is None


# ---------------------------------------------------------------- rendering


def test_format_transcript_includes_speakers_and_timestamps():
    text = format_transcript([Segment(0, 1, "hi", "Alice"), Segment(65, 66, "bye", "Bob")])
    assert text == "[00:00] Alice: hi\n[01:05] Bob: bye"


def test_build_notes_with_llm(monkeypatch):
    monkeypatch.setattr(
        pipeline,
        "_llm_summary",
        lambda transcript, settings: {
            "title": "Planning",
            "summary": "We planned.",
            "action_items": ["Ship"],
            "topics": ["plan"],
        },
    )
    title, md = build_notes(
        recording_id="20260101-000000-abcd",
        segments=[Segment(0, 1, "hi")],
        language="en",
        duration=60,
        device="dev",
        settings=Settings(),
    )
    assert title == "Planning"
    assert "## Summary" in md and "We planned." in md
    assert "- Ship" in md
    assert "plan" in md


def test_build_notes_empty_recording():
    title, md = build_notes(
        recording_id="20260101-000000-abcd",
        segments=[],
        language="en",
        duration=0,
        device="dev",
        settings=Settings(),
    )
    assert title.startswith("Recording")
    assert "_No speech detected._" in md


# ------------------------------------------------------------ full pipeline


def test_process_recording_writes_all_artifacts(tmp_path, monkeypatch):
    settings = Settings(data_dir=tmp_path / "data", vault_dir=tmp_path / "vault")
    settings.ensure_dirs()
    wav = make_wav(settings.data_dir / "20260101-000000-abcd" / "audio.wav", 0.5)
    monkeypatch.setattr(
        pipeline, "transcribe", lambda path, s: ([Segment(0, 1, "hello", "Alice")], "en"))
    monkeypatch.setattr(pipeline, "_llm_summary", lambda t, s: None)
    meta = process_recording(
        recording_id="20260101-000000-abcd",
        wav_path=wav,
        device="dev",
        duration=0.5,
        settings=settings,
    )
    out = wav.parent
    for name in ("transcript.txt", "transcript.json", "notes.md", "meta.json"):
        assert (out / name).exists()
    assert Path(meta["published"]).exists()


def test_process_recording_survives_broken_vault(tmp_path, monkeypatch):
    blocker = tmp_path / "file"
    blocker.write_text("x")
    settings = Settings(data_dir=tmp_path / "data", vault_dir=blocker / "vault")
    settings.ensure_dirs()
    wav = make_wav(settings.data_dir / "20260101-000000-abcd" / "audio.wav", 0.2)
    monkeypatch.setattr(pipeline, "transcribe", lambda path, s: ([Segment(0, 1, "hi")], "en"))
    monkeypatch.setattr(pipeline, "_llm_summary", lambda t, s: None)
    meta = process_recording(
        recording_id="20260101-000000-abcd",
        wav_path=wav,
        device="dev",
        duration=0.2,
        settings=settings,
    )
    assert meta["published"] is None
    assert (wav.parent / "notes.md").exists()


def test_process_recording_runs_diarization_when_enabled(tmp_path, monkeypatch):
    settings = Settings(data_dir=tmp_path / "data", diarize=True, hf_token="x")
    settings.ensure_dirs()
    wav = make_wav(settings.data_dir / "20260101-000000-abcd" / "audio.wav", 0.2)
    monkeypatch.setattr(pipeline, "transcribe", lambda path, s: ([Segment(0, 1, "hi")], "en"))
    monkeypatch.setattr(pipeline, "_llm_summary", lambda t, s: None)
    monkeypatch.setattr(pipeline, "_load_diarizer", lambda s: (lambda path: FakeAnnotation()))
    meta = process_recording(
        recording_id="20260101-000000-abcd",
        wav_path=wav,
        device="dev",
        duration=0.2,
        settings=settings,
    )
    transcript = (wav.parent / "transcript.txt").read_text()
    assert "SPEAKER_0" in transcript
    assert meta["notes"]
