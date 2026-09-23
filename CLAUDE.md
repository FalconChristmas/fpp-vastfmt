# fpp-vastfmt

An FPP plugin for Si471x FM transmitters, over two quite different transports:

- **USB** — a VAST Electronics V-FMT212R. An MCU behind USB HID fronts the
  chip. **The part inside is an Si4711**, which is why every function in
  `VASTFMT.cpp` is named `sendSi4711Command`.
- **I2C** — a bare **Si4713** module on the I2C bus, plus a GPIO wired to its
  reset pin.

It broadcasts whatever audio FPP plays and sends RDS (PS and RadioText).

## Layout

| File | What it is |
|---|---|
| `src/FPPVastFM.cpp` | The plugin: worker thread, settings, RDS text, HTTP API |
| `src/Si4713.cpp/.h` | Shared command layer — tuning, power, RDS, properties |
| `src/VASTFMT.cpp/.h` | USB HID transport (the V-FMT212R adapter) |
| `src/I2CSi4713.cpp/.h` | I2C transport, including the reset-pin sequence |
| `src/bitstream.c/.h` | RDS bit packing |
| `settings.json` | Setting metadata: types, ranges, tooltips, Advanced gating |
| `plugin_setup.php` | Settings page, status panel, station-text preview |
| `scripts/fpp_install.sh`, `fpp_uninstall.sh` | Lifecycle (build in place) |
| `callbacks.sh` | **Must stay in the repo root** — see below |

## Architecture

**One worker thread owns the transmitter.** Playlist and media callbacks,
setting changes and HTTP handlers queue work; nothing else touches the device.
That keeps slow I2C and USB exchanges off fppd's main loop and off drogon
threads, and stops two exchanges overlapping.

Unload safety depends on the order in `shutdown()`: withdraw the HTTP routes
first (that call does not return until no request is executing and the handler
has been destroyed, so nothing can queue more work), then stop and join the
worker, then close the device. A queued job must never reach the caller's
stack — a wait that times out does not cancel the job — so jobs write into the
shared `RadioJob` they are handed and capture only `this`.

`callbacks.sh` stays in the root because FPP looks for it only at
`<plugindir>/<name>/callbacks[.sh|.pl|.php|.py]` — there is no `scripts/`
fallback — and the `c++` it prints for `--list` is what makes FPP load the
shared library at all. `fpp_install.sh` *is* found in `scripts/`.

## Build and compatibility

**FPP 10+ only** — the status API uses `registerPluginApi()` and drogon. The
FPP 9 entry in `pluginInfo.json` is pinned with `allowUpdates: 0`; leave it
frozen, or FPP 9 boxes get offered an update that cannot compile.

On BeagleBone-class hardware (one core, ~480 MB) always build through the
distributed compiler. A plain `make` thrashes the box, and the wrapper falls
back to local compilation *silently* if it cannot reach its servers — confirm
it actually offloaded.

## The two transports are not interchangeable

This is where most of the surprises live.

- **Part numbers differ.** `GET_REV` reports **11 (Si4711)** on the USB adapter
  and **13 (Si4713)** on bare modules. Check the 10–13 family, never `== 13` —
  another FPP plugin for this chip does exactly that and would reject the USB
  hardware outright.
- **`TX_TUNE_STATUS` through the USB raw passthrough returns zeros.** Read the
  tune status via the adapter's own request instead (`readTuneStatus()` does the
  right thing per transport). This also makes STCINT unusable over USB: the flag
  is only cleared by `TX_TUNE_STATUS` with INTACK, so it stays latched from the
  previous tune and any wait on it returns immediately.
- **`TX_TUNE_MEASURE` (received-noise scan) is Si4713-only.** It returns varying
  noise levels over I2C and answers with the error bit set on the Si4711.

## Tuning behaviour

- **The status readback lags a tune by ~140 ms** and reports the *previous*
  tune until it catches up. Reading it straight away is how a retune came to
  report the last session's frequency and a railed antenna cap. Wait for the
  readback to report what was asked for; do not sleep a guess.
- After a **power/antenna-cap** change there is no new frequency to compare
  against, and the stale reading is itself perfectly stable while the chip
  works — so "poll until it stops moving" locks onto the old value. Ride out the
  lag first, *then* require stability.
- **`AntCap` 0 runs the chip's automatic search**; any other value pins the
  capacitor and turns the search off. `TX_TUNE_FREQ` re-runs the search too, not
  just `TX_TUNE_POWER`. When the antenna is not resonant near the frequency the
  search rails to the end of its range — that is the chip reporting no match,
  and it is reported rather than hidden.
- **Power** is `0` (PA off) or 88–120 dBµV. Values outside that are clamped.

## Is the module good or bad?

1. **Part number.** `GET_REV` must return 10–13. The plugin refuses to start
   without one and says so, because a transmitter whose I2C side answers but
   whose core never powers up otherwise "succeeds" at everything while
   broadcasting nothing.
2. **The classic dead module**, seen and confirmed on two different boards:
   - ACKs at its I2C address and returns status `0x80` (CTS) right after reset,
     so the I2C side looks perfectly healthy
   - **`POWER_UP` never returns CTS** — status drops to `0x00` and stays there
   - fails identically with `XOSCEN` set or clear, at every settle time, with
     retries, **and for `FUNC=15` (query library ID)** — which is the most
     minimal power-up there is, with no transmit path involved
   - `GET_REV` then reads part number 0, and an invalid command sets no ERR bit
   - a power cycle does not help

   That is a dead part, not a driver or antenna problem. The Si471x has separate
   digital and analog supplies, so it can talk I2C while its core never starts.
   Swap the module.
3. **An I2C module only answers while its reset line is driven high.** If
   nothing drives it, the chip does not appear in `i2cdetect` at all — absence
   there is *not* evidence of a dead part. Driving the pin low and watching the
   address vanish is a good way to confirm you have the right pin.
4. **GPIO lines are exclusive, and that is a real failure mode.** FPP holds the
   reset line for as long as the plugin is loaded — `gpioinfo` shows it as
   `output consumer="FPPD"`. If anything else already has it (a leftover
   `gpioset`, a hand-rolled hold service, a previous process), FPP cannot drive
   it, the chip stays in reset, and nothing before the part-number check
   notices. A working module in this state looks exactly like a dead one, so
   check `gpioinfo | grep -i consumer` before suspecting the hardware.
5. **Garbage or shifting values from every register** usually means something
   else owns the bus. fppd holds the device whenever the plugin is loaded —
   *stop fppd before running any standalone probe.*

## Playlist callbacks

FPP sends `action == "start"` **only** when the player was idle. A playlist
started while another is running, "Start Playlist At Item", and advancing
sections all arrive as `"playing"` — `Playlist.cpp` picks between them with
`origStatus == FPP_STATUS_PLAYLIST_PLAYING`, and several call sites send
`"playing"` unconditionally. `"start"` itself comes from the media path, not
the playlist one.

Anything keyed to starting must therefore accept both. Matching only `"start"`
is why *Start at: Playlist Start* did nothing for a user while *FPPD Start*
worked perfectly: the callback fired every time, just never with the word the
code was looking for, and nothing was logged to say so. The callback now logs
its action, which is what makes this visible in a log at all.

## Reset pin

I2C only. The setting is a menu built from the board's own pin list. Cape
defaults may ship it as a **bare GPIO number** rather than a pin name (one cape
uses `"2"`), which the page translates to a name on first render — otherwise the
menu has nothing to match and saving would silently point reset elsewhere. An
unknown pin name yields a null pin; that is checked, because dereferencing it
used to take fppd down while a show was running.

## Conventions

- **URLs in the page and `settings.json` must be relative**
  (`api/plugin-apis/vastfmt`, not `/api/...`). A leading slash breaks the FPP
  proxy and FPPMon, and in `optionsURL` it is read as a local file path and
  kills the page mid-render.
- Settings live in `settings.json` with tooltips and `level: 1` for anything
  fiddly; the page renders them with `PrintSettingGroup`. `PrintSetting` emits
  `id=` and an inline `onChange`, but **no `name=`** — look fields up by id and
  *add* listeners rather than replacing FPP's own.
- Setting *values* are compared against string literals in the C++
  (`"FPPDStart"`, `"True"`, `"50us"`, `"I2C"`, `"1"`). Changing an option value
  in `settings.json` silently changes behaviour; check the comparisons.
- Commit messages, comments and release notes carry no host names, IP
  addresses, local paths or show/sequence names.
