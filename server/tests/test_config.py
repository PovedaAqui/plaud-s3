"""Environment parsing for Settings."""

from app.config import Settings, _flag


def test_flag_accepts_common_truthy_values(monkeypatch):
    for value in ("1", "true", "yes", "on"):
        monkeypatch.setenv("PLAUD_DIARIZE", value)
        assert _flag("PLAUD_DIARIZE") is True


def test_flag_defaults_and_falsey_values(monkeypatch):
    monkeypatch.delenv("PLAUD_DIARIZE", raising=False)
    assert _flag("PLAUD_DIARIZE") is False
    monkeypatch.setenv("PLAUD_DIARIZE", "no")
    assert _flag("PLAUD_DIARIZE") is False


def test_settings_reads_numeric_and_optional_values(monkeypatch):
    monkeypatch.setenv("PLAUD_ASR_BEAM_SIZE", "7")
    monkeypatch.setenv("PLAUD_LLM_TEMPERATURE", "0.5")
    monkeypatch.setenv("PLAUD_LLM_MAX_TOKENS", "128")
    monkeypatch.setenv("PLAUD_LANGUAGE", "")
    settings = Settings()
    assert settings.asr_beam_size == 7
    assert settings.llm_temperature == 0.5
    assert settings.llm_max_tokens == 128
    assert settings.language is None


def test_settings_language_override(monkeypatch):
    monkeypatch.setenv("PLAUD_LANGUAGE", "pt")
    assert Settings().language == "pt"


def test_extra_body_parsing(monkeypatch):
    monkeypatch.setenv("PLAUD_LLM_EXTRA_BODY", '{"chat_template_kwargs": {"enable_thinking": false}}')
    assert Settings().llm_extra_body == {"chat_template_kwargs": {"enable_thinking": False}}


def test_extra_body_invalid_json_is_ignored(monkeypatch):
    monkeypatch.setenv("PLAUD_LLM_EXTRA_BODY", "not json")
    assert Settings().llm_extra_body == {}
    monkeypatch.setenv("PLAUD_LLM_EXTRA_BODY", "[1,2]")
    assert Settings().llm_extra_body == {}
