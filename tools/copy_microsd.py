"""Copy a mounted microSD item from the Module over an existing ADB connection."""

import argparse
from pathlib import Path, PurePosixPath
import socket
import subprocess
import tempfile


MOUNT = '/run/muse-storyteller/sd'
MODULE = 'axera-ax620e'


def copy(adb, port, item, destination):
    relative = PurePosixPath(item)
    if (relative.is_absolute() or not relative.parts or '\\' in item or ':' in item
            or any(part.startswith('.') for part in relative.parts)):
        raise ValueError('Use a relative card path such as Muse Exports/Muse-12345678')
    destination = Path(destination).resolve()
    if not destination.is_dir():
        raise ValueError('Choose an existing PC folder for the copy')
    final = destination / relative.name
    if final.exists():
        raise ValueError('The destination already exists; choose another folder')
    with socket.create_connection(('127.0.0.1', port), timeout=2):
        pass
    base = [adb, '-H', '127.0.0.1', '-P', str(port), '-s', MODULE]
    table = subprocess.check_output(base + ['shell', 'cat /proc/self/mountinfo'], text=True, timeout=10)
    for line in table.splitlines():
        before, sep, after = line.partition(' - ')
        if sep and before.split()[4] == MOUNT and after.split()[0] in ('vfat', 'exfat'):
            break
    else:
        raise ValueError('Mount the microSD from Settings before copying')
    with tempfile.TemporaryDirectory(prefix='muse-sd-', dir=destination) as temporary:
        target = Path(temporary) / relative.name
        subprocess.run(base + ['pull', MOUNT + '/' + relative.as_posix(), str(target)], check=True)
        if final.exists():
            raise ValueError('The destination appeared during the copy; choose another folder')
        target.rename(final)
    return final


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adb', default='adb', help='Path to the ADB client for the existing server')
    parser.add_argument('--port', type=int, default=5037)
    parser.add_argument('--item', default='Muse Exports', help='Relative file or folder on the card')
    parser.add_argument('destination', help='Existing PC destination folder')
    args = parser.parse_args()
    try:
        print(copy(args.adb, args.port, args.item, args.destination))
    except (ValueError, OSError, subprocess.SubprocessError) as exc:
        parser.exit(1, f'Copy failed: {exc}\n')
