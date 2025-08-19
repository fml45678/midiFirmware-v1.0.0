madMidi — README

Notes about deep sleep and wake behavior (Seeed XIAO S3)

Sleep/wake diagnostics added to sketch:

- The sketch prints an "EXT1 wake pins mask" and the digital state of each `WAKE_PINS` before enabling EXT1.
- Before entering deep sleep, the sketch prints "Preparing to enter deep sleep..." and "Final EXT1 mask: 0x..." and flushes Serial.
- The sketch attempts to keep the RTC peripheral powered and detaches USB (when supported) to improve deep sleep reliability.
- On wake the sketch prints the numeric wake reason and the EXT1 wakeup status mask.

Common failure modes and mitigations:

- USB/TinyUSB stack can prevent deep sleep. Test by unplugging USB and verifying sleep behavior. If sleep works without USB, use the USB detach or shut down the USB/MIDI interfaces before sleeping.
- Internal pull-ups in the RTC domain may not be strong enough on some hardware or toolchains. Use external pull-ups (10k) on wake pins if you see immediate wake.
- Potentiometers or wiring can couple to wake pins. Try testing with fewer wake pins (single pin) and/or add external pull-ups.

Tips:

- To make debug iteration faster, temporarily reduce `sleepTimeout` in `madMidi.ino`.
- Keep `DEBUG_NO_SLEEP` defined while troubleshooting to keep Serial alive (comment it out to actually sleep).

If you want, I can add an explicit Control Surface USB/MIDI shutdown sequence to further improve USB-related sleep reliability.

TODO / Next improvements

- Move `LittleFS.begin()` to `setup()` and only mount once at boot (reduces repeated mounts and flash wear).
- Add stricter config validation when applying JSON (validate MIDI channel 1-16, CC 0-127, note 0-127) and reject/save only valid configs.
- Improve SysEx ACK handling: send structured ACK/NAK responses and have the web UI wait for a confirmation before showing success.

Testing notes:

- After moving `LittleFS.begin()` to `setup()`, watch Serial on boot for any filesystem mount errors and verify `/config.json` still gets written by SysEx/Serial uploads.
- For validation, attempt to upload malformed JSON or out-of-range values and confirm the device rejects them and does not overwrite `/config.json`.
- For ACK handling, test with the web UI and check that the client waits for the SysEx ACK sequence before reporting success.
