# Installation boundary

This is a **source preview of a hardware-tested integration**, not a factory installer.
The maintainer’s Module already had working StackFlow services and a verified ownership
handoff from its previous application. That state is deliberately not published.

## Before changing a device

1. Identify **Core2 v1.1**, the Module firmware/services, and each USB data port.
2. Save your own Core2 flash/NVS backup and Module runtime/configuration backup privately.
3. Finish Recorder saving and stop or explicitly pause narration before maintenance.
4. Preserve pairing, Wi-Fi, existing application backups and service ownership records.

No tool in this repository erases flash, writes NVS, starts an ADB server, replaces drivers,
or installs a service automatically. The firmware itself saves settings and recordings when used.

## Prepare and build

See [BUILD.md](BUILD.md). The public build excludes the maintainer’s location and custom animated Muse avatar,
and disables screenshot/bench controls by default. It therefore is not byte-identical to
the private reference application that produced the development hardware reports.

The Core2 firmware and Module service must agree on the `musevoice.v1` protocol.
The legacy model names in the handshake are compatibility identifiers; actual recognition
is the English Whisper Base adapter and actual narration uses Piper.

## Module prerequisites

See [MODULE.md](MODULE.md). Run `python3 module/doctor.py` on the Module for a read-only
resource report. Inspect missing resources and vendor versions before enabling the service.
A failed report is a blocker, not permission to upgrade the whole vendor framework.

## Flashing

Use the pinned upstream’s Core2-v1.1 build/flash instructions and inspect the generated
partition map. Existing compatible installations can update the application separately;
new boards may need the complete bootloader/partition/application set. These are different
procedures. Do not assume one device’s offsets are correct for another layout.

After flashing, confirm a single healthy boot, live UI, module-ready status, speech capture,
narration and touch controls. A successful write digest alone does **not** establish a healthy boot.
