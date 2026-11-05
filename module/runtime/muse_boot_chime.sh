#!/bin/sh
# /run is cleared at boot. mkdir is atomic across simultaneous invocations.
# Keep the factory asset and playback parameters, allowing one cue per boot.
if ! mkdir /run/muse-startup-chime.once 2>/dev/null; then
    exit 0
fi
exec tinyplay -D0 -d1 /usr/local/m5stack/logo.wav
