import argparse
import subprocess
import sys
import time
from pathlib import Path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--port', required=True)
    ap.add_argument('--baud', default='115200')
    ap.add_argument('--esptool', required=True)
    ap.add_argument('--framework', required=True)
    ap.add_argument('--build-dir', required=True)
    ap.add_argument('--retries', type=int, default=8)
    args = ap.parse_args()

    build = Path(args.build_dir)
    bootloader = build / 'bootloader.bin'
    partitions = build / 'partitions.bin'
    firmware = build / 'firmware.bin'
    boot_app0 = Path(args.framework) / 'tools' / 'partitions' / 'boot_app0.bin'

    required = [bootloader, partitions, firmware, boot_app0]
    missing = [str(x) for x in required if not x.exists()]
    if missing:
        print('[CYD upload] Missing binaries:', *missing, sep='\n  ', file=sys.stderr)
        return 2

    cmd = [
        sys.executable, args.esptool,
        '--chip', 'esp32', '--port', args.port, '--baud', str(args.baud),
        '--before', 'default_reset', '--after', 'hard_reset',
        'write_flash', '-z', '--flash_mode', 'dio', '--flash_freq', '40m', '--flash_size', '4MB',
        '0x1000', str(bootloader),
        '0x8000', str(partitions),
        '0xe000', str(boot_app0),
        '0x10000', str(firmware),
    ]

    for attempt in range(1, args.retries + 1):
        print(f'\n[CYD upload] Attempt {attempt}/{args.retries} on {args.port} @ {args.baud}...')
        rc = subprocess.run(cmd).returncode
        if rc == 0:
            print('[CYD upload] Flash completed and verified.')
            return 0
        if attempt < args.retries:
            print('[CYD upload] Connection/flash failed; retrying reset sequence...')
            time.sleep(0.8)

    print('[CYD upload] All retry attempts failed.', file=sys.stderr)
    return 1


if __name__ == '__main__':
    raise SystemExit(main())
