#!/bin/sh
# /run is cleared at boot. Claim it through Python's direct atomic syscall;
# do not depend on a platform mkdir command's concurrent-exit behavior.
# Keep the factory asset and playback parameters, allowing one cue per boot.
if ! python3 -c 'import os; os.mkdir("/run/muse-startup-chime.once", 0o700)' 2>/dev/null; then
    exit 0
fi
exec tinyplay -D0 -d1 /usr/local/m5stack/logo.wav
