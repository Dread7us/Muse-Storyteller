# Muse allowance: estimate versus sync

The Module anchors the estimate to the last verified Muse percentage. It counts outgoing
and incoming characters and outgoing turns from locally saved Muse exchanges. The estimator
stores only counts and calibration readings. It uses up to 28 recent calibration intervals,
gives more weight to recent evidence, and keeps learned rates across weekly resets. Small
unchanged readings can accumulate before they affect the fit, and readings at 100% do not
train it. Upgraded installations start with their previous combined rate and build new
directional history from subsequent exchanges.

Repeated increases during periods with zero device traffic can support a bounded estimate
of account activity outside those exchanges. This allowance estimate uses at most one day
of that projected activity per reading. Muse’s own reading is the source for spending and
limit decisions. Browser and phone activity, tool work, and provider billing weights can
still move the verified meter differently from the estimate.

## Is the estimate improving?

Starting in 0.6.1, each device check takes a snapshot of the displayed percentage before
the request goes out. When a verified reading arrives, the Module saves both numbers and
the signed error in percentage points. A negative error means the estimate was low. The
popup shows the latest comparable result and the mean absolute error of up to five checks.
The full report compares the most recent five with the preceding five once ten comparable
checks are available. Lower mean absolute error means smaller misses in that sample.

The report retains up to 120 verified checks locally. The first check without an estimate,
checks that cross a reset, and older clients that supply only a pre-apply estimate are
excluded from the trend. Repeated deliveries of the same reading count once. The separate
desktop alert monitor does not feed these device accuracy records.

On the Module, run `python3 -B /opt/muse-core2-bridge/muse_usage.py --accuracy` to read
the report. It contains percentages and timestamps. Keep your report private.

The popup shows estimated remaining allowance, the last confirmed Muse percentage,
reset date, and last successful sync. A failed check retains the last good observation.

## Check from the device

Tap **Check now** in Muse account usage. The Core2 asks the paired Muse to use
its own subscription-status tool in a separate chat and fetches that request’s
small private result over the existing paired connection. The Module rejects
stale or invalid results and confirms the applied reading. The device also
checks when online and idle if its last successful reading is a day old.

The device uses the consumer Muse account already paired to it. Each check is
a short Muse request that may count toward the account’s allowance. A failed or
exhausted Muse session leaves the local estimate available with its last sync date.
The Gadget SDK does not document a direct consumer allowance endpoint.

## Legacy Windows helper setup

Use only one usage helper per Module. Existing installations should keep their working
helper; do not register a second copy alongside it.

1. In Chrome's Extensions page, enable Developer mode and load `integrations/chrome`
   as an unpacked extension. Note its extension ID.
2. In a Visual Studio C/C++ developer shell with Python installed, preview registration:
   `python tools/setup_chrome_helper.py --extension-id YOUR_EXTENSION_ID --adb PATH_TO_EXISTING_ADB_EXE`.
3. Review the preview, then repeat with `--register` to create a local native launcher
   and register its per-user Chrome host. Configuration stays in ignored `private/`.
4. On editions through 0.5.11, sign into Muse in Chrome yourself, connect the
   Module's data port, and use Sync via PC. Check the success result and timestamp.

Registration does not load extensions, sign in, start ADB or change SuperDisplay.
This portable helper source has not undergone a separate fresh-PC installation test.

The UI disables repeated requests while checking, uses a generous timeout, and offers
Retry when confirmation does not arrive. “Synced successfully” follows a verified applied
reading; the date is the last successful transfer, not the time the popup was opened.
