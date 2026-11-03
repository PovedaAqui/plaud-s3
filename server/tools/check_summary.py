#!/usr/bin/env python3
"""Check the note-generation path against an OpenAI-compatible endpoint.

    PLAUD_LLM_BASE_URL=http://127.0.0.1:8080/v1 \
    PLAUD_LLM_MODEL=minicpm-v-4.7-1b \
        python tools/check_summary.py [transcript.txt]

Exits 1 when the endpoint returns no usable summary, so it can gate a smoke run.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent))

from app.config import Settings
from app.pipeline import _llm_summary

SAMPLE = """[00:00] Alice: Let's review the launch readiness.
[00:04] Bob: Firmware is done except the battery drain test.
[00:09] Alice: Can you finish that by Friday?
[00:12] Bob: Yes, I'll run the drain test tomorrow morning.
[00:17] Carol: I'll prepare the release notes and send them to marketing.
[00:23] Alice: Decision: we ship on the 14th if the drain test passes.
[00:29] Bob: I'll also file the FCC paperwork this week.
[00:34] Carol: I'll book the support training for next Tuesday."""


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("transcript", nargs="?", help="text file; built-in sample otherwise")
    parser.add_argument("--url", default=None, help="override PLAUD_LLM_BASE_URL")
    parser.add_argument("--model", default=None, help="override PLAUD_LLM_MODEL")
    args = parser.parse_args()

    text = pathlib.Path(args.transcript).read_text() if args.transcript else SAMPLE
    settings = Settings()
    if args.url:
        object.__setattr__(settings, "llm_base_url", args.url)
    if args.model:
        object.__setattr__(settings, "llm_model", args.model)

    print(f"endpoint {settings.llm_base_url} model {settings.llm_model}")
    notes = _llm_summary(text, settings)
    if not notes:
        print("FAIL: no summary returned")
        return 1

    print(json.dumps(notes, indent=2, ensure_ascii=False))
    print(f"OK: {len(notes.get('action_items', []))} action item(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
