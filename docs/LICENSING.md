# Code, models, and attribution

## Project and upstream source

Edition application code is offered under [Apache-2.0](../LICENSE), except files retaining
their own notices. The source overlay is based on Meta’s Apache-2.0 Muse Gadget SDK at
the revision recorded in `firmware/upstream.json`. Original headers are retained.

The LVGL renderer patch is a modification of MIT-licensed LVGL source. Generated reader
fonts use bundled LVGL font material. The generated-code basis retains LVGL MIT terms;
Montserrat and Font Awesome font material retain their respective SIL OFL notices.
Full notices are included under `third_party/`; these fonts are not newly Apache-licensed.
The overlay does not include the maintainer’s avatar or publish upstream artwork as newly
Apache-licensed content. Upstream assets fetched into `.local/sdk` retain their own terms.

## External assets are not project code

| Asset | Source / terms |
|---|---|
| StackFlow | [M5Stack StackFlow](https://github.com/m5stack/StackFlow) — MIT source; packaged components can have additional notices. |
| Whisper | [OpenAI Whisper](https://github.com/openai/whisper) — original source/model license upstream; check M5Stack’s converted packages separately. |
| Piper distribution | [rhasspy/piper](https://github.com/rhasspy/piper) — tested archived runtime; its archive includes dependencies with their own licenses. Newer Piper projects can use different licenses. |
| Lessac-low voice | [Model card](https://huggingface.co/rhasspy/piper-voices/blob/main/en/en_US/lessac/low/MODEL_CARD) — links to the [Blizzard 2013 research license](https://www.cstr.ed.ac.uk/projects/blizzard/2013/lessac_blizzard2013/license.html). |
| Local reviewer | Existing M5Stack Qwen model/runtime — review their respective terms. |
| Forecasts | [Open-Meteo](https://open-meteo.com/en/terms) — service and attribution requirements apply. |

“No paid speech API” means inference runs on the user’s hardware, not that every model
has unrestricted redistribution or commercial-use rights. In particular, the Lessac model
card’s linked dataset terms restrict research materials; this repository makes no broader
rights claim about the weights. Obtain and review assets directly from their publishers.

No voice weights, vendor package archives, executables, personal recordings, or firmware
flash images are distributed in this repository. `module/assets.json` records identifiers,
download links and checksums for reproducibility; it does not relicense the downloads.

## Splash branding

The optional splash uses a compact display encoding of the owner's public generated
avatar. The encoded image data in `muse_splash_art.c` is a separate branding asset,
excluded from the Apache code license; its integration/descriptor code remains project
code. It is not upstream Muse artwork. No personal Muse avatar renderer or pairing
state is needed to build it.

## Device screenshots

The README gallery contains four reviewed v0.2.0 device UI captures in
`docs/assets/screenshots/`. They show project UI and branding, with no account-usage
readout or conversation/recording contents. Screenshot publication does not relicense
third-party marks or the separately licensed splash branding described above.
