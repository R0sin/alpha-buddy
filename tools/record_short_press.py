"""Record short-press diagnostics without sending commands or resetting the device.

Example: python tools/record_short_press.py --port COM9 --seconds 60 \
    --output .pio/short-press/af-c.jsonl

Count actual new photos on the camera separately. 'control_completed' confirms
control acknowledgements, not a saved photo. 'control_failed' does not prove
that no photo was taken.
"""

import argparse
import json
import time
from pathlib import Path

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--seconds", type=float, default=60)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.seconds <= 0:
        parser.error("--seconds must be positive")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    port = serial.Serial(port=None, baudrate=115200, timeout=0.2)
    port.port = args.port
    port.dtr = port.rts = False
    events = results = 0
    started = time.monotonic()
    # Exclusive creation preserves earlier hardware evidence.
    with args.output.open("x", encoding="utf-8") as output, port:
        while time.monotonic() - started < args.seconds:
            try:
                event = json.loads(port.readline())
            except (ValueError, UnicodeDecodeError):
                continue
            if not isinstance(event, dict):
                continue
            if event.get("event") not in {
                "shutter_submitted", "shutter_state", "shutter_result", "shutter_cancelled"
            }:
                continue
            event["host_elapsed_ms"] = round((time.monotonic() - started) * 1000)
            line = json.dumps(event, ensure_ascii=False)
            output.write(line + "\n")
            output.flush()
            print(line, flush=True)
            events += 1
            results += event["event"] == "shutter_result"
    print(f"Recorded {events} diagnostic events, {results} control results. "
          "Actual photo count must be checked on the camera.")
    if not events:
        raise SystemExit("No shutter diagnostics received; this is not a successful hardware test.")


if __name__ == "__main__":
    main()
