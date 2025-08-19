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
