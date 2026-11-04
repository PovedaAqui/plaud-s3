"""Send a WAV file to the ingest server as if it came from the device.

Usage: python tools/send_wav.py sample.wav [ws://127.0.0.1:8000/ws/audio]
"""

from __future__ import annotations

import asyncio
import json
import sys
import wave

import websockets


async def main(path: str, url: str) -> None:
    with wave.open(path) as wav:
        assert wav.getsampwidth() == 2, "expects 16-bit PCM"
        assert wav.getnchannels() == 1, "expects mono"
        rate = wav.getframerate()
        pcm = wav.readframes(wav.getnframes())
        duration = wav.getnframes() / rate

    async with websockets.connect(url, max_size=None) as ws:
        await ws.send(json.dumps({"type": "start", "device": "wav-client", "rate": rate}))
        greeting = json.loads(await ws.recv())
        while greeting.get("type") == "time":
            greeting = json.loads(await ws.recv())
        print("server:", greeting)

        chunk = rate * 2 // 10  # 100 ms
        for offset in range(0, len(pcm), chunk):
            await ws.send(pcm[offset : offset + chunk])
            await asyncio.sleep(0.02)  # faster than real time, still paced

        await ws.send(json.dumps({"type": "stop"}))
        print(f"sent {duration:.1f}s;", "server:", await ws.recv())


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    asyncio.run(main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else "ws://127.0.0.1:8000/ws/audio"))
