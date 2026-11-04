# Maintaining the edition

The public repository deliberately contains the edition layer rather than a second copy
of every Android/iOS/Linux SDK file. Upstream remains the authority for the foundation.

## Update procedure

1. Make a branch and review the upstream diff from the pinned SDK commit.
2. Use a separate candidate checkout. Rebase/merge the corresponding source edits there;
   do not overwrite an edited `.local/sdk` with a new overlay.
3. Update `firmware/upstream.json` only after reviewing every changed base file. Its
   hashes are change detectors, not an invitation to blindly replace expected values.
   After reviewing the overlay changes, run `python tools/update_overlay_manifest.py`.
   It normalizes source files to LF (matching `.gitattributes` and GitHub checkouts)
   and refreshes overlay hashes without changing the pinned upstream/base hashes.
4. Recheck dependency versions and the LVGL patch’s before/after hashes.
5. Run public checks and an isolated Core2-v1.1 source build.
6. Validate the real hardware lifecycle, backed up first, before publishing new hardware claims.

The preparation helper refuses unknown base hashes and preserves existing work. It
does not merge source conflicts, reset a checkout, flash a board, push a branch or update
the running Module.

## Publication hygiene

Run `python tools/check_public_tree.py` before a commit. Review `git diff --cached` as well;
the scanner is a useful guard, not proof that every possible secret format is detected.

Keep these local: SDK/account tokens, browser sessions, native-host registration/config,
weather coordinates, Wi-Fi/NVS images, ownership ledgers, SQLite archives, recordings,
logs, model downloads, and complete device backups. Never upload a full workspace or
`/var/lib` tree as a convenient release archive.

## Edition versions

`firmware/overlay/esp32/components/muse/muse_storyteller_version.h` is the single
source of the on-device edition version. **Increment it for every shipped firmware
update**: patch for fixes, minor for features. Update the README and CHANGELOG for that
release. Test-only rebuilds do not need a bump. Keep this version separate from the
upstream SDK/OTA version; changing an OTA tag has different compatibility implications.
