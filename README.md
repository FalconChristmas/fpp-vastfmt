# fpp-vastfmt

RDS and audio control for Si4713-based FM transmitters, for
[Falcon Player (FPP)](https://github.com/FalconChristmas/fpp).

Keeps the transmitter's RDS text in step with whatever FPP is playing, so listeners see the current
song on their car radio, and exposes the Si4713's audio processing settings.

## Supported hardware

Two connection types, selectable in the UI:

- **USB** — Vast Electronics V-FMT212R USB FM transmitter (driven over USB HID).
- **I2C** — bare Si4713 breakout modules, wired to the I2C bus with a GPIO pin for reset.

## Features

- RDS station text and RadioText, updated as the playlist advances.
- Transmit frequency, power, and antenna capacitance (AntCap) control.
- Preemphasis selection.
- Audio limiter, audio compression (with threshold), and audio gain.
- Start/stop control of the transmitter, plus a program type (PTY) setting.

## Installation

Install from **Content Setup → Plugins** in the FPP web UI, then restart FPPD.

## Configuration

**Input/Output Setup → Vast FMT212** in the FPP web UI.

- *Connection* — `USB` for the V-FMT212R, or `I2C` for an Si4713 module.
- *Reset Pin* — GPIO pin wired to the module's reset line (I2C connections only). The pin list is
  populated from the pins FPP knows about on the current board, so the default differs between
  Raspberry Pi and BeagleBone.
- *Frequency*, *Power*, *AntCap*, *Preemphasis* — RF settings.
- *Audio Limitter*, *Audio Compression*, *Audio Compression Threshold*, *Audio Gain* — the
  Si4713's built-in audio processing.
- *Enable RDS*, *Station Text*, *RDS Text*, *Pty* — RDS content and program type.

## License

GPLv2 — see [LICENSE](LICENSE).
