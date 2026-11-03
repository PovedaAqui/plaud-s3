"""Transcription, optional diarization, and note generation."""

from __future__ import annotations

import json
import logging
import re
import urllib.error
import urllib.request
import wave
from dataclasses import asdict, dataclass
from datetime import datetime
from pathlib import Path

import numpy as np

from .config import Settings

log = logging.getLogger(__name__)

_whisper = None
_whisper_key: tuple[str, str, str] | None = None


@dataclass
class Segment:
    start: float
    end: float
    text: str
    speaker: str = "Speaker 1"

    @property
    def stamp(self) -> str:
        total = int(self.start)
        return f"{total // 60:02d}:{total % 60:02d}"


def _get_whisper(settings: Settings):
    """Load the ASR model once per process; reload only if settings change."""
    global _whisper, _whisper_key
    key = (settings.asr_model, settings.asr_device, settings.asr_compute)
    if _whisper is None or _whisper_key != key:
        from faster_whisper import WhisperModel

        log.info("loading faster-whisper model %s on %s (%s)", *key)
        _whisper = WhisperModel(key[0], device=key[1], compute_type=key[2])
        _whisper_key = key
    return _whisper


def read_pcm(wav_path: Path) -> np.ndarray:
    """Decode our own 16 kHz mono WAV without an ffmpeg dependency."""
    with wave.open(str(wav_path)) as wav:
        if wav.getsampwidth() != 2 or wav.getnchannels() != 1:
            raise ValueError("expected 16-bit mono WAV")
        rate = wav.getframerate()
        raw = wav.readframes(wav.getnframes())
    samples = np.frombuffer(raw, dtype="<i2").astype(np.float32) / 32768.0
    if rate != 16000:
        raise ValueError(f"expected 16 kHz audio, got {rate}")
    return samples


def transcribe(wav_path: Path, settings: Settings) -> tuple[list[Segment], str]:
    model = _get_whisper(settings)
    iterator, info = model.transcribe(
        read_pcm(wav_path),
        language=settings.language,
        vad_filter=settings.asr_vad,
        beam_size=settings.asr_beam_size,
    )
    segments = [
        Segment(start=float(s.start), end=float(s.end), text=s.text.strip())
        for s in iterator
        if s.text.strip()
    ]
    return segments, info.language


def _load_diarizer(settings: Settings):
    from pyannote.audio import Pipeline

    if not settings.hf_token:
        raise RuntimeError("PLAUD_HF_TOKEN is required for diarization")
    pipeline = Pipeline.from_pretrained(
        "pyannote/speaker-diarization-community-1",
        token=settings.hf_token,
    )
    return pipeline


def diarize(wav_path: Path, segments: list[Segment], settings: Settings) -> list[Segment]:
    """Assign speaker labels by maximum temporal overlap. Never fails the job."""
    try:
        pipeline = _load_diarizer(settings)
        annotation = pipeline(str(wav_path))
    except Exception as exc:  # optional feature: degrade, do not abort
        log.warning("diarization unavailable: %s", exc)
        return segments

    turns = [
        (turn.start, turn.end, str(label))
        for turn, _, label in annotation.itertracks(yield_label=True)
    ]
    for seg in segments:
        measured: dict[str, float] = {}
        for start, end, label in turns:
            overlap = min(seg.end, end) - max(seg.start, start)
            if overlap > 0:
                measured[label] = measured.get(label, 0.0) + overlap
        if measured:
            seg.speaker = max(measured, key=measured.get)
    return segments


def _llm_chat(prompt: str, settings: Settings) -> dict | None:
    """POST a single prompt to an OpenAI-compatible endpoint and parse JSON."""
    payload = {
        "model": settings.llm_model,
        "messages": [{"role": "user", "content": prompt}],
        "temperature": settings.llm_temperature,
        "stream": False,
    }
    if settings.llm_max_tokens > 0:
        payload["max_tokens"] = settings.llm_max_tokens
    body = json.dumps(payload).encode()
    request = urllib.request.Request(
        settings.llm_base_url.rstrip("/") + "/chat/completions",
        data=body,
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {settings.llm_api_key}",
        },
    )
    try:
        with urllib.request.urlopen(request, timeout=settings.llm_timeout) as response:
            payload = json.loads(response.read())
        content = payload["choices"][0]["message"]["content"].strip()
    except (urllib.error.URLError, KeyError, IndexError, json.JSONDecodeError, TimeoutError) as exc:
        log.warning("LLM request failed: %s", exc)
        return None

    match = re.search(r"\{.*\}", content, re.DOTALL)
    if not match:
        return None
    try:
        data = json.loads(match.group(0))
    except json.JSONDecodeError:
        return None
    return data if isinstance(data, dict) else None


def _llm_summary(transcript: str, settings: Settings) -> dict | None:
    """Two small calls instead of one large one: a 1B model follows short,
    single-purpose prompts far more reliably, and a malformed action-items
    answer cannot cost us the summary."""
    excerpt = transcript[: settings.llm_max_chars]
    notes = _llm_chat(
        "Summarise this meeting transcript. Reply with JSON only, keys: "
        '"title" (3-8 words describing the topic, never a quote), '
        '"summary" (3-6 sentences using only facts from the transcript), '
        '"topics" (3-8 short strings). Do not invent names, numbers or commitments.'
        "\n\nTranscript:\n" + excerpt,
        settings,
    )
    if not notes or not notes.get("summary"):
        return None

    actions = _llm_chat(
        "From this transcript, list tasks a participant explicitly committed to "
        '(phrases like "I will" / "I\'ll" or a direct assignment). '
        'Reply with JSON only: {"action_items": ["..."]}. '
        "Use an empty array when there is no such commitment. "
        "Suggestions, questions and decisions are not actions."
        "\n\nTranscript:\n" + excerpt,
        settings,
    )
    if actions and isinstance(actions.get("action_items"), list):
        notes["action_items"] = [str(x) for x in actions["action_items"] if str(x).strip()]
    else:
        notes.setdefault("action_items", [])
    return notes


def _slug(text: str) -> str:
    cleaned = re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")
    return cleaned[:60] or "recording"


def format_transcript(segments: list[Segment]) -> str:
    return "\n".join(f"[{s.stamp}] {s.speaker}: {s.text}" for s in segments)


def build_notes(
    *,
    recording_id: str,
    segments: list[Segment],
    language: str,
    duration: float,
    device: str,
    settings: Settings,
) -> tuple[str, str]:
    """Return (title, markdown). Summary omitted when no LLM is reachable."""
    plain = format_transcript(segments)
    notes = _llm_summary(plain, settings) if segments else None

    default_title = f"Recording {recording_id}"
    title = (notes or {}).get("title") or default_title
    lines = [
        f"# {title}",
        "",
        f"- Date: {recording_id[:4]}-{recording_id[4:6]}-{recording_id[6:8]}",
        f"- Duration: {duration / 60:.1f} min",
        f"- Device: {device}",
        f"- Language: {language}",
        f"- Segments: {len(segments)}",
        "",
    ]
    if notes:
        lines += ["## Summary", "", str(notes["summary"]).strip(), ""]
        actions = notes.get("action_items") or []
        if actions:
            lines += ["## Action items", ""] + [f"- {a}" for a in actions] + [""]
        topics = notes.get("topics") or []
        if topics:
            lines += ["## Topics", "", ", ".join(map(str, topics)), ""]
    elif segments:
        lines += [
            "## Summary",
            "",
            (
                "_No language model was reachable, so no summary was generated. "
                "The transcript below is unmodified._"
            ),
            "",
        ]

    lines += ["## Transcript", "", plain or "_No speech detected._", ""]
    return title, "\n".join(lines)


def process_recording(
    *,
    recording_id: str,
    wav_path: Path,
    device: str,
    duration: float,
    settings: Settings,
) -> dict:
    """Transcribe, optionally diarize, write artifacts, optionally publish to the vault."""
    segments, language = transcribe(wav_path, settings)
    if settings.diarize and segments:
        segments = diarize(wav_path, segments, settings)

    title, markdown = build_notes(
        recording_id=recording_id,
        segments=segments,
        language=language,
        duration=duration,
        device=device,
        settings=settings,
    )

    out_dir = wav_path.parent
    (out_dir / "transcript.txt").write_text(format_transcript(segments), encoding="utf-8")
    (out_dir / "transcript.json").write_text(
        json.dumps([asdict(s) for s in segments], indent=2, ensure_ascii=False),
        encoding="utf-8",
    )
    notes_path = out_dir / "notes.md"
    notes_path.write_text(markdown, encoding="utf-8")

    published: Path | None = None
    if settings.vault_dir:
        try:
            publish_dir = settings.vault_dir / "Plaud"
            publish_dir.mkdir(parents=True, exist_ok=True)
            published = publish_dir / f"{recording_id}-{_slug(title)}.md"
            published.write_text(markdown, encoding="utf-8")
        except OSError as exc:  # a broken vault path must not lose the recording
            log.warning("vault publish failed: %s", exc)
            published = None

    meta = {
        "id": recording_id,
        "device": device,
        "duration": round(duration, 2),
        "language": language,
        "segments": len(segments),
        "title": title,
        "notes": str(notes_path),
        "published": str(published) if published else None,
        "finished_at": datetime.now().isoformat(timespec="seconds"),
    }
    (out_dir / "meta.json").write_text(json.dumps(meta, indent=2), encoding="utf-8")
    return meta
