"""Request one read-only 0x9209 snapshot from the property-probe firmware."""
import argparse
import json
import time
from pathlib import Path

import serial


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM9')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    data = bytearray()
    started = False
    lines = []
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = args.port, 115200, 1
    port.dtr = port.rts = False
    with port:
        port.reset_input_buffer()
        port.write(b'p')
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            line = port.readline()
            lines.append(line)
            try:
                event = json.loads(line)
            except (ValueError, UnicodeDecodeError):
                continue
            if event.get('event') != 'property_snapshot':
                continue
            if event['kind'] == 'begin':
                if started:
                    raise RuntimeError('Duplicate snapshot begin')
                started = True
            elif event['kind'] == 'data':
                if not started or event['offset'] != len(data):
                    raise RuntimeError('Missing or reordered snapshot chunk')
                data.extend(bytes.fromhex(event['hex']))
            elif event['kind'] == 'end':
                if not (started and event['transport_ok'] and
                        event['response'] == 0x2001 and
                        event['bytes'] == len(data) and data):
                    args.output.parent.mkdir(parents=True, exist_ok=True)
                    args.output.with_suffix('.failed.log').write_bytes(b''.join(lines))
                    raise RuntimeError(f'Incomplete snapshot: {event}, received={len(data)}, started={started}')
                args.output.parent.mkdir(parents=True, exist_ok=True)
                with args.output.open('xb') as output:
                    output.write(data)
                print(f'Saved {len(data)} bytes to {args.output}')
                return
        raise TimeoutError('No complete snapshot; check camera connection and probe firmware')


if __name__ == '__main__':
    main()
