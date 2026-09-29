"""Collect firmware's five-second windows; fail on missing data or regression.

Requires pyserial (included with PlatformIO). Keep the scene and lighting fixed
between runs. Capture FPS counts enqueued frames; display FPS counts completed
draws. Display age starts at a complete, validated JPEG, not sensor exposure.
"""
import argparse
import json
import time
from pathlib import Path

import serial


def evaluate(windows, display_windows, *, min_fps=0, max_width=65535,
             width=None, height=None, min_display_fps=None, max_display_age_ms=None):
    frames = sum(w["frames"] for w in windows)
    duration = sum(w["window_ms"] for w in windows)
    if frames <= 0 or duration <= 0:
        return False, "no captured frames"
    fps = 1000 * frames / duration
    jpeg_bytes = sum(w["jpeg_avg_bytes"] * w["frames"] for w in windows) / frames
    passed = (fps >= min_fps and
              all(0 < w["jpeg_width"] <= max_width and w["frame_failures"] == 0
                  and (width is None or w["jpeg_width"] == width)
                  and (height is None or w["jpeg_height"] == height) for w in windows))
    summary = f"capture {fps:.2f} fps, {jpeg_bytes:.0f} bytes/frame, {frames} frames"
    if min_display_fps is not None or max_display_age_ms is not None:
        displayed = sum(w["frames"] for w in display_windows)
        display_duration = sum(w["window_ms"] for w in display_windows)
        if displayed <= 0 or display_duration <= 0:
            return False, summary + "; no display measurements"
        display_fps = 1000 * displayed / display_duration
        age = sum(w["frames"] * w["ready_to_display_avg_ms"]
                  for w in display_windows) / displayed
        passed = (passed and all(w["display_failures"] == 0 for w in display_windows)
                  and (min_display_fps is None or display_fps >= min_display_fps)
                  and (max_display_age_ms is None or age <= max_display_age_ms))
        summary += f"; display {display_fps:.2f} fps, JPEG-ready-to-display {age:.2f} ms"
    return passed, summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--windows", type=int, default=6)
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--skip-windows", type=int, default=1)
    parser.add_argument("--min-fps", type=float, default=0)
    parser.add_argument("--max-width", type=int, default=65535)
    parser.add_argument("--width", type=int)
    parser.add_argument("--height", type=int)
    parser.add_argument("--min-display-fps", type=float)
    parser.add_argument("--max-display-age-ms", type=float,
                        help="maximum mean JPEG-ready-to-display age, not end-to-end latency")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.windows < 1 or args.timeout <= 0 or args.skip_windows < 0:
        parser.error("windows and timeout must be positive; skip-windows must be nonnegative")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    windows = []
    display_windows = []
    require_display = args.min_display_fps is not None or args.max_display_age_ms is not None
    seen = {"live_view_perf": 0, "display_perf": 0}
    with serial.Serial(args.port, 115200, timeout=1) as port, args.output.open("w", encoding="utf-8") as log:
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline and (len(windows) < args.windows or
                                               (require_display and len(display_windows) < args.windows)):
            line = port.readline().decode("utf-8", "replace").strip()
            if not line:
                continue
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue
            # Store only diagnostic events, never configuration/credential data.
            if not isinstance(event, dict) or event.get("event") not in (
                    "live_view_perf", "live_view_quality", "display_perf"):
                continue
            log.write(json.dumps(event) + "\n")
            log.flush()
            print(json.dumps(event), flush=True)
            kind = event["event"]
            if kind in seen:
                seen[kind] += 1
                target = windows if kind == "live_view_perf" else display_windows
                if seen[kind] > args.skip_windows and len(target) < args.windows:
                    target.append(event)
    if len(windows) != args.windows:
        raise SystemExit(f"FAIL: only {len(windows)}/{args.windows} windows received")
    if require_display and len(display_windows) != args.windows:
        raise SystemExit(f"FAIL: only {len(display_windows)}/{args.windows} display windows received")
    passed, summary = evaluate(windows, display_windows, min_fps=args.min_fps,
                               max_width=args.max_width, width=args.width, height=args.height,
                               min_display_fps=args.min_display_fps,
                               max_display_age_ms=args.max_display_age_ms)
    print(f"{'PASS' if passed else 'FAIL'}: {summary}")
    raise SystemExit(0 if passed else 1)


if __name__ == "__main__":
    main()
