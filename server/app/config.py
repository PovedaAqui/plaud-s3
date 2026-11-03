"""Configuration for the Plaud-S3 capture server."""

from __future__ import annotations

import json
import os
from dataclasses import dataclass, field
from pathlib import Path


def _json_env(name: str) -> dict:
    raw = os.environ.get(name)
    if not raw:
        return {}
    try:
        value = json.loads(raw)
    except json.JSONDecodeError:
        return {}
    return value if isinstance(value, dict) else {}


def _flag(name: str, default: bool = False) -> bool:
    value = os.environ.get(name)
    if value is None:
        return default
    return value.strip().lower() in {"1", "true", "yes", "on"}


@dataclass(frozen=True)
class Settings:
    data_dir: Path = field(default_factory=lambda: Path(os.environ.get("PLAUD_DATA_DIR", "./data")).resolve())
    vault_dir: Path | None = field(
        default_factory=lambda: Path(os.environ["PLAUD_VAULT_DIR"]).resolve()
        if os.environ.get("PLAUD_VAULT_DIR")
        else None
    )

    asr_model: str = field(default_factory=lambda: os.environ.get("PLAUD_ASR_MODEL", "small"))
    asr_device: str = field(default_factory=lambda: os.environ.get("PLAUD_ASR_DEVICE", "cpu"))
    asr_compute: str = field(default_factory=lambda: os.environ.get("PLAUD_ASR_COMPUTE", "int8"))
    language: str | None = field(
        default_factory=lambda: (os.environ.get("PLAUD_LANGUAGE") or None)
    )
    asr_beam_size: int = field(
        default_factory=lambda: int(os.environ.get("PLAUD_ASR_BEAM_SIZE", "5"))
    )
    asr_vad: bool = field(default_factory=lambda: _flag("PLAUD_ASR_VAD", True))

    diarize: bool = field(default_factory=lambda: _flag("PLAUD_DIARIZE"))
    hf_token: str | None = field(default_factory=lambda: os.environ.get("PLAUD_HF_TOKEN") or None)

    llm_base_url: str = field(
        default_factory=lambda: os.environ.get("PLAUD_LLM_BASE_URL", "http://127.0.0.1:11434/v1")
    )
    llm_model: str = field(default_factory=lambda: os.environ.get("PLAUD_LLM_MODEL", "llama3.2"))
    llm_api_key: str = field(default_factory=lambda: os.environ.get("PLAUD_LLM_API_KEY", "ollama"))
    llm_timeout: float = field(default_factory=lambda: float(os.environ.get("PLAUD_LLM_TIMEOUT", "8")))
    llm_max_chars: int = field(
        default_factory=lambda: int(os.environ.get("PLAUD_LLM_MAX_CHARS", "24000"))
    )
    llm_temperature: float = field(
        default_factory=lambda: float(os.environ.get("PLAUD_LLM_TEMPERATURE", "0.2"))
    )
    llm_max_tokens: int = field(
        default_factory=lambda: int(os.environ.get("PLAUD_LLM_MAX_TOKENS", "0"))
    )
    # Extra JSON merged into every chat request, e.g. provider-specific switches.
    llm_extra_body: dict = field(
        default_factory=lambda: _json_env("PLAUD_LLM_EXTRA_BODY")
    )

    # Process inline instead of in a background task (used by tests).
    sync_pipeline: bool = field(default_factory=lambda: _flag("PLAUD_SYNC_PIPELINE"))

    def ensure_dirs(self) -> None:
        self.data_dir.mkdir(parents=True, exist_ok=True)


settings = Settings()
