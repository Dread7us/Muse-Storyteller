# Build the firmware

Use Git, Python, and an activated **ESP-IDF v6.0.1** environment. The board build was tested
with the ESP32 target and Core2-v1.1 overlays. Work in a path your toolchain supports.

```sh
python tools/prepare_firmware.py
python tools/build_firmware.py
```

Preparation clones the public SDK at the revision in `firmware/upstream.json`, verifies
every base file, then copies the edition overlay into `.local/sdk`. Existing edited
checkouts are not reset. To reuse an existing **upstream Git source**, without copying
its uncommitted files:

```sh
python tools/prepare_firmware.py --source /path/to/muse-gadget-sdk
```

The build helper runs component resolution, verifies/applies the LVGL text-progress patch,
and builds `.local/build/muse-gadget.bin`. It never flashes a device. If the component
hash changes, review the patch rather than bypassing its check.

## Keep your configuration private

The SDK may require a personal gadget SDK token for pairing. Obtain your own from the
official Muse gadget account page. Set it through ESP-IDF menuconfig or a local
`private/sdkconfig.secrets` overlay; that entire directory is ignored. Do not paste real
tokens into a command you intend to publish, an issue, the checked-in board profile, or a README.

Weather is off until `private/weather.json` exists. Its schema is:

```json
{
  "latitude": 0.0,
  "longitude": 0.0,
  "label": "Your chosen location"
}
```

The coordinates above are a **schema example**, not a recommended location. Replace them
locally. The build helper generates an ignored private weather header inside `.local/sdk`.
It requests Fahrenheit, mph and inches with the provider’s automatic timezone selection.
Review [Open-Meteo’s terms](https://open-meteo.com/en/terms) for your use.

## Checks

```sh
python tools/check_public_tree.py
python -m unittest discover -s tests -v
```

A native C compiler is required for the explicit-command C test. It runs with GCC/Clang
on Linux in CI; Python-only checks can also run on Windows. Tests use synthetic text and
temporary directories, not a real conversation database, microphone or model.
