madMidi firmware - notes

What changed

- Buttons are grouped into `BUTTON_PINS[]` and a `NoteButton buttons[]` array.
- Potentiometers are grouped into a `pots[]` array.
- LED handling is centralized with `setAllLEDs()` which avoids redundant FastLED.show() calls.
- A `MIDI_CHANNEL` constant centralizes the channel assignment for all controls.

Pin mapping (current)

- LEDs: DATA_PIN = 42, NUM_LEDS = 4 (WS2812B chain)
- Buttons (GPIO): 7, 8, 9, 39, 40, 41, 44
- Pots (analog pins): 1, 2, 3, 4, 5, 6

Sleep / Wake notes

- `goToSleep()` currently constructs an EXT1 wakeup mask; not all GPIOs are RTC-wakeup capable on every ESP32 board.
- Before enabling EXT1 wakeup, confirm which board you're using so we can:
  - compute the correct wakeup mask from `BUTTON_PINS[]`, and
  - only include RTC-capable pins.

Next steps (I can do)

- Update deep-sleep wakeup mask using the board's RTC-capable pins (need to know your board).
- Add a DEBUG compile flag for serial logging.
- Add per-pin comments and a diagram for PCB wiring.

How to test locally

- Compile with Arduino IDE or PlatformIO with these libraries installed: FastLED, Control Surface, and ESP-IDF/ESP32 headers.
- If the editor shows missing includes, install the libraries or configure the includePath for IntelliSense.
# midiFirmware-v1.0.0
