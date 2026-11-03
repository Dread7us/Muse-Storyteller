# Muse allowance: estimate versus sync

The Module keeps a local estimate anchored to the last verified website percentage.
It can learn a rough percentage-per-character rate from later observations. This is
**not Muse’s billing formula** and does not account for every other client between checks.

The popup shows estimated remaining allowance, the last website-used percentage, reset
label, last successful sync, and an explicit request outcome. A failed check retains the
last good observation. The estimate continues locally when the device leaves the PC.

## Fresh website checks

The reference installation used a locally registered Chrome extension/native-messaging
helper on Windows. A fresh check requires:

1. Module LLM connected through its own USB data port.
2. The existing ADB server; target only the Module’s serial.
3. Chrome open, signed into the correct Muse account, with the helper configured.

The helper opens a new Muse tab, reads only the visible Settings usage card, validates
that its percentage and reset label agree, applies the observation, then checks that the
Module acknowledged it. It must not copy browser credentials or infer zero from a loading card.

This source release includes the Module-side usage queue and estimate, plus optional
Chrome/native-host helper source. It excludes registered identities, account sessions,
local configuration and scheduled automation. Without local setup, Sync via PC can time
out; it is not a standalone Wi-Fi account query. No public, supported direct-usage API
is claimed here.

## Optional Windows helper setup

Use only one usage helper per Module. Existing installations should keep their working
helper; do not register a second copy alongside it.

1. In Chrome's Extensions page, enable Developer mode and load `integrations/chrome`
   as an unpacked extension. Note its extension ID.
2. In a Visual Studio C/C++ developer shell with Python installed, preview registration:
   `python tools/setup_chrome_helper.py --extension-id YOUR_EXTENSION_ID --adb PATH_TO_EXISTING_ADB_EXE`.
3. Review the preview, then repeat with `--register` to create a local native launcher
   and register its per-user Chrome host. Configuration stays in ignored `private/`.
4. Sign into Muse in Chrome yourself, connect the Module's data port, and use the
   device's Sync via PC button. Check the explicit success result and timestamp.

Registration does not load extensions, sign in, start ADB or change SuperDisplay.
This portable helper source has not undergone a separate fresh-PC installation test.

The UI disables repeated requests while checking, uses a generous timeout, and offers
Retry when confirmation does not arrive. “Synced successfully” follows a verified applied
reading; the date is the last successful transfer, not the time the popup was opened.
