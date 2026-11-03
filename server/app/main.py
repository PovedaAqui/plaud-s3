"""FastAPI ingest server for the ESP32-S3 recorder."""

from __future__ import annotations

import asyncio
import json
import logging
from datetime import datetime
from pathlib import Path

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import JSONResponse

from .config import settings
from .pipeline import process_recording
from .wavio import WavWriter

log = logging.getLogger("plaud")
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")

app = FastAPI(title="plaud-s3", version="0.1.0")

# Active writers keyed by websocket so two devices can record at once.
_active: dict[WebSocket, dict] = {}


def _recording_id() -> str:
    return datetime.now().strftime("%Y%m%d-%H%M%S")


@app.get("/health")
async def health() -> dict:
    return {"ok": True, "active": len(_active), "data_dir": str(settings.data_dir)}


@app.get("/recordings")
async def recordings() -> list[dict]:
    items = []
    for meta in sorted(settings.data_dir.glob("*/meta.json"), reverse=True):
        try:
            items.append(json.loads(meta.read_text()))
        except json.JSONDecodeError:
            continue
    return items


@app.get("/recordings/{recording_id}")
async def recording(recording_id: str) -> JSONResponse:
    meta = settings.data_dir / recording_id / "meta.json"
    if not meta.exists():
        return JSONResponse({"error": "not found"}, status_code=404)
    return JSONResponse(json.loads(meta.read_text()))


async def _finish(ws: WebSocket, state: dict, started_at: float) -> None:
    if state.get("finished"):
        return
    state["finished"] = True
    state["writer"].close()
    duration = state["writer"].duration
    log.info("recording %s finished: %.1fs from %s", state["id"], duration, state["device"])

    kwargs = dict(
        recording_id=state["id"],
        wav_path=state["wav"],
        device=state["device"],
        duration=duration,
        settings=settings,
    )
    meta = None
    if settings.sync_pipeline:
        try:
            meta = await asyncio.to_thread(process_recording, **kwargs)
        except Exception:
            log.exception("pipeline failed for %s", state["id"])
    else:
        asyncio.create_task(_process_later(**kwargs))

    try:
        await ws.send_json(
            {
                "type": "done",
                "id": state["id"],
                "duration": round(duration, 2),
                "notes": meta["notes"] if meta else None,
            }
        )
    except Exception:
        pass  # device already disconnected
    if meta:
        log.info("notes: %s", meta["notes"])


async def _process_later(**kwargs) -> None:
    try:
        meta = await asyncio.to_thread(process_recording, **kwargs)
        log.info("notes: %s", meta["notes"])
    except Exception:  # keep the audio even if the pipeline fails
        log.exception("pipeline failed for %s", kwargs["recording_id"])


@app.websocket("/ws/audio")
async def audio_ws(ws: WebSocket) -> None:
    await ws.accept()
    state: dict | None = None
    try:
        while True:
            message = await ws.receive()
            if message["type"] == "websocket.disconnect":
                break

            if message.get("text") is not None:
                try:
                    payload = json.loads(message["text"])
                except json.JSONDecodeError:
                    await ws.send_json({"type": "error", "error": "bad json"})
                    continue

                kind = payload.get("type")
                if kind == "start":
                    if state:
                        await ws.send_json({"type": "error", "error": "already recording"})
                        continue
                    recording_id = _recording_id()
                    out_dir = settings.data_dir / recording_id
                    out_dir.mkdir(parents=True, exist_ok=True)
                    wav_path = out_dir / "audio.wav"
                    state = {
                        "id": recording_id,
                        "device": str(payload.get("device", "esp32"))[:64],
                        "wav": wav_path,
                        "writer": WavWriter(wav_path),
                        "started_at": asyncio.get_running_loop().time(),
                    }
                    _active[ws] = state
                    log.info("recording %s started from %s", recording_id, state["device"])
                    await ws.send_json({"type": "started", "id": recording_id})
                elif kind == "stop":
                    if not state:
                        await ws.send_json({"type": "error", "error": "not recording"})
                        continue
                    _active.pop(ws, None)
                    await _finish(ws, state, state["started_at"])
                    state = None
                elif kind == "ping":
                    await ws.send_json({"type": "pong"})
                continue

            if message.get("bytes") is not None and state:
                state["writer"].write(message["bytes"])
    except WebSocketDisconnect:
        pass
    finally:
        if state:
            _active.pop(ws, None)
            try:
                await _finish(ws, state, state["started_at"])
            except Exception:
                log.exception("failed to finalise %s", state["id"])
