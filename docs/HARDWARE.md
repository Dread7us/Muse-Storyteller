# Hardware and power

The reference device is an **M5Stack Core2 v1.1 + Module LLM (M140 / AX630C)**.
The Module provides the microphone and speaker in this profile; Core2 I2S audio is not initialized.
Its UART link uses TX14/RX13 at 115200 baud. Follow M5Stack’s assembly guidance rather than changing wiring from this description alone.

The Core2 v1.1 uses AXP2101 power management. Core2 v1.0 uses a different controller;
do not substitute its firmware profile. Other M5 hosts and the newer LLM630 ecosystem are not validated here.

## Power is separate from battery percentage

The header battery indicator describes **Core2’s battery**, not a battery inside Module LLM.
In the tested arrangement, turning on battery-powered Core2 did not boot the Module.
Supply the Module with stable external power through the supported hardware arrangement.
Two powered devices do not imply two PC data connections: a suitable external power supply
can power Module LLM when it is away from the maintenance PC.

LLM Mate is optional hardware and is not used by this integration. The tested models,
transcripts and archives fit on the Module’s onboard eMMC; no microSD is required.
This does not provide unlimited recording storage. Recorder imposes storage budgets and
can continue text capture when optional audio storage reaches its budget.

## Maintenance USB

- Core2’s USB connection is used for firmware and local diagnostics.
- Module LLM’s **own** USB-C data port is used for its ADB connection.
- The stack connector does not replace the Module’s USB data connection to the PC.
- Shared ADB clients must target a serial explicitly. Never use a broad `adb kill-server`
  as a routine fix when other devices or display software depend on the server.

Official references:

- [Core2 v1.1](https://docs.m5stack.com/en/core/Core2_v1.1)
- [Module LLM](https://docs.m5stack.com/en/module/Module_LLM)
- [M5Unified](https://github.com/m5stack/M5Unified)
