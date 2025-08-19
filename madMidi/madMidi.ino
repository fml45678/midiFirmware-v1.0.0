  /** 
 * Written by PieterP, 2019-08-07  
 * https://github.com/tttapa/Control-Surface
 */
#include <FastLED.h>
constexpr int NUM_LEDS = 4;
constexpr int DATA_PIN = 42;
CRGB leds[NUM_LEDS];

// Uncomment to disable actual deep sleep for debugging wake logic (keeps Serial alive)
// #define DEBUG_NO_SLEEP

#include <Control_Surface.h> // Include the Control Surface library
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <driver/rtc_io.h>
// Filesystem for storing config
#include <LittleFS.h>
#include <FS.h>
// JSON parsing
#include <ArduinoJson.h>

// Centralized MIDI channel used by buttons and pots. Change here to reassign all controls.
constexpr auto MIDI_CHANNEL = CHANNEL_3;

// Instantiate a MIDI over USB interface or Bluetooth.
USBMIDI_Interface midi;
BluetoothMIDI_Interface bmidi;
HardwareSerialMIDI_Interface serialmidi {Serial1, MIDI_BAUD};

BidirectionalMIDI_PipeFactory<3> pipes;

// Inactivity timer
unsigned long lastActivityTime = 0;
constexpr unsigned long sleepTimeout = 60000; // 4 minutes in ms


// Button and wake pin definitions
// Hardware pin mapping
constexpr int BUTTON_PINS[] = {7, 8, 9, 39, 40, 41, 44};
// Wake pins to use for EXT1 wake on this board (Seeed XIAO S3) - user requested
constexpr int WAKE_PINS[] = {7, 8, 9};

// Runtime control containers (constructed in setup() from config or defaults)
NoteButton* buttons[7] = {nullptr};
CCPotentiometer* pots[6] = {nullptr};

// Default mapping data (used if no config present)
constexpr int DEFAULT_BUTTON_NOTES[7] = {
  MIDI_Notes::C(4), MIDI_Notes::D(4), MIDI_Notes::E(4), MIDI_Notes::G(4),
  MIDI_Notes::A(4), MIDI_Notes::B(4), MIDI_Notes::C(5)
};
constexpr int DEFAULT_POT_CCS[6] = {
  MIDI_CC::Channel_Volume, MIDI_CC::Pan, MIDI_CC::Modulation_Wheel,
  MIDI_CC::Portamento_Time, MIDI_CC::Balance, MIDI_CC::Effect_Control_1
};

// Simple helper: find "<id>" in json and extract the following "channel" number.
int extractChannelFromJson(const String &json, const char *id, int fallback) {
  String key = String("\"") + id + "\"";
  int idx = json.indexOf(key);
  if (idx < 0) return fallback;
  int chIdx = json.indexOf("\"channel\"", idx);
  if (chIdx < 0) return fallback;
  int colon = json.indexOf(':', chIdx);
  if (colon < 0) return fallback;
  // read number after colon
  int i = colon + 1;
  // skip spaces
  while (i < json.length() && isSpace(json[i])) ++i;
  int val = 0;
  bool found = false;
  while (i < json.length() && isDigit(json[i])) {
    found = true;
    val = val * 10 + (json[i] - '0');
    ++i;
  }
  return found ? val : fallback;
}



// Track current LED color to avoid redundant FastLED.show() calls
CRGB currentLEDColor = CRGB::Black;

// Helper: set all LEDs to the same color and show (only when color changed)
void setAllLEDs(const CRGB &color) {
  if (color == currentLEDColor) return; // no change, skip update
  for (int i = 0; i < NUM_LEDS; ++i) {
    leds[i] = color;
  }
  FastLED.show();
  currentLEDColor = color;
}

// Helper: mark activity (reset inactivity timer)
void markActivity() {
  lastActivityTime = millis();
}

// Helper: handle a NoteButton's falling/rising events and update LEDs + activity
void handleButtonLED(NoteButton &btn) {
  if (btn.getButtonState() == Button::Falling) {
    setAllLEDs(CRGB::Red);
    markActivity();
  } else if (btn.getButtonState() == Button::Rising) {
    setAllLEDs(CRGB::Black);
    markActivity();
  }
}

void goToSleep() {
  // Turn off LEDs before sleeping
  setAllLEDs(CRGB::Black);
  
  // Disable WiFi and Bluetooth to save power
  esp_wifi_stop();
  esp_bt_controller_disable();
  
  // Create a bitmask for all wake-up pins from BUTTON_PINS[]
  // Note: some GPIOs (for example GPIO44) are not RTC/EXT1-wake capable on many boards.
  // We exclude known non-wakeable pins here. If your hardware differs, update the
  // `NON_WAKEABLE_PINS` list below.
  // Before building the wake mask, enable internal pull-ups on the wake pins
  // so they present a stable HIGH level when not pressed (EXT1 wakes on LOW).
  for (int i = 0; i < (int)(sizeof(WAKE_PINS)/sizeof(WAKE_PINS[0])); ++i) {
    int pin = WAKE_PINS[i];
    pinMode(pin, INPUT_PULLUP);
  // Also enable the RTC-domain internal pull-up so the level survives deep sleep
  // (EXT1 uses RTC IO pull-ups, not the regular GPIO pull-ups on some chips)
  rtc_gpio_pullup_en((gpio_num_t)pin);
  }

  auto buildWakeMask = []() -> uint64_t {
    // Conservative exclusion list for pins known to be non-RTC-wake on many boards.
    // Update this array if your board differs.
    constexpr int NON_WAKEABLE_PINS[] = {44};
    uint64_t mask = 0;
    for (int i = 0; i < (int)(sizeof(WAKE_PINS)/sizeof(WAKE_PINS[0])); ++i) {
      int pin = WAKE_PINS[i];
      bool excluded = false;
      for (int e = 0; e < (int)(sizeof(NON_WAKEABLE_PINS)/sizeof(NON_WAKEABLE_PINS[0])); ++e) {
        if (pin == NON_WAKEABLE_PINS[e]) { excluded = true; break; }
      }
      if (!excluded) {
        if (pin >= 0 && pin < 64) mask |= (1ULL << pin);
      }
    }
    return mask;
  };

  uint64_t pin_mask = buildWakeMask();
  // Debug: print which pins will be used for wake
  Serial.print("EXT1 wake pins mask: 0x");
  Serial.println((unsigned long)pin_mask, HEX);
  // Print the live digital state of each wake pin to help diagnose immediate wake
  for (int i = 0; i < (int)(sizeof(WAKE_PINS)/sizeof(WAKE_PINS[0])); ++i) {
    int p = WAKE_PINS[i];
    int v = digitalRead(p);
    Serial.print("Pin "); Serial.print(p);
    Serial.print(" state before sleep: "); Serial.println(v);
  }
  if (pin_mask == 0) {
    Serial.println("[WARN] No wake-capable pins included in mask. Not entering deep sleep.");
    return;
  }
  
  // ESP_EXT1_WAKEUP_ANY_LOW means wake up when any pin goes LOW
  // This is typically what you want for buttons/encoders that pull to ground
  esp_sleep_enable_ext1_wakeup(pin_mask, ESP_EXT1_WAKEUP_ANY_LOW);
  
  // Enter deep sleep
  // Visual indicator: flash LEDs briefly before sleeping
  setAllLEDs(CRGB::Blue);
  delay(100);
  setAllLEDs(CRGB::Black);
  // Give logs time to be seen before the device goes away
  Serial.println("Preparing to enter deep sleep...");
  Serial.print("Final EXT1 mask: 0x");
  Serial.println((unsigned long)pin_mask, HEX);
  Serial.flush();

  // Ensure RTC-peripheral domain stays powered so internal RTC pull-ups remain active
  // This increases the chance EXT1 wake will work on some boards (including S3 variants)
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);

  // If USB is present, try to detach it so the USB stack doesn't interfere with deep sleep.
  // These calls are wrapped in feature-detection macros so they only compile on platforms
  // that provide the USBDevice/TinyUSB APIs.
#ifdef USBCON
  Serial.println("Detaching USBDevice (USBCON)");
  USBDevice.detach();
#endif
#ifdef TINYUSB_ENABLED
  Serial.println("Calling tud_disconnect() (TinyUSB)");
  tud_disconnect();
#endif

#ifndef DEBUG_NO_SLEEP
  // Small delay to let USB detach complete (if applicable) and for Serial to flush
  delay(20);
  Serial.flush();
  // This call should not return
  esp_deep_sleep_start();
#else
  Serial.println("[DEBUG] Skipping deep sleep (DEBUG_NO_SLEEP defined)");
#endif
  // If we reach here, esp_deep_sleep_start() returned unexpectedly.
  Serial.println("[ERROR] esp_deep_sleep_start() returned — deep sleep did not start");
  Serial.flush();
  // Halt here to avoid continuing to send MIDI or repeat the sleep sequence.
  while (true) {
    // Blink the LEDs slowly to indicate error state
    setAllLEDs(CRGB::Orange);
    delay(250);
    setAllLEDs(CRGB::Black);
    delay(1750);
  }

}

// Function to reinitialize after wake
void handleWakeup() {
  // Check wake up reason
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  Serial.print("Wakeup reason (numeric): ");
  Serial.println((int)wakeup_reason);
  
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT1) {
    // Woke up from button/encoder press
    uint64_t wakeup_pin_mask = esp_sleep_get_ext1_wakeup_status();
    // Map wakeup_pin_mask back to pins and print which of our BUTTON_PINS caused the wake
    Serial.print("EXT1 wakeup status mask: 0x");
    Serial.println((unsigned long)wakeup_pin_mask, HEX);
    for (int i = 0; i < (int)(sizeof(WAKE_PINS)/sizeof(WAKE_PINS[0])); ++i) {
      int pin = WAKE_PINS[i];
      if (pin >= 0 && pin < 64) {
        if (wakeup_pin_mask & (1ULL << pin)) {
          Serial.print("Woke by pin: ");
          Serial.println(pin);
          // Visual wake indicator
          setAllLEDs(CRGB::Green);
          delay(100);
          setAllLEDs(CRGB::Black);
        }
      }
    }
  }
  
  // Re-enable Bluetooth controller
  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  esp_bt_controller_init(&bt_cfg);
  esp_bt_controller_enable(ESP_BT_MODE_BLE);
  
  // WiFi will be re-initialized automatically by the WiFi library if needed
}

// --- Configuration receive / persistence ---
// We'll accept config via SysEx over USB-MIDI and as newline-terminated JSON over Serial.
String sysexBuffer;
bool receivingSysEx = false;

void applyConfigJson(const String &json) {
  Serial.print("Received config JSON length: "); Serial.println(json.length());
  // Persist to LittleFS
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed");
    return;
  }
  File f = LittleFS.open("/config.json", FILE_WRITE);
  if (!f) {
    Serial.println("Failed to open config file for writing");
    return;
  }
  f.print(json);
  f.close();
  Serial.println("Config written to /config.json");
  // Apply immediately
  constructControlsFromConfig(json);

  // Send a simple SysEx ACK back: [F0 7D 02 01 F7]
#if defined(usbMIDI)
  byte ack[] = {0xF0, 0x7D, 0x02, 0x01, 0xF7};
  // Attempt to send on the USBMIDI interface
  midi.send(ack, sizeof(ack));
#endif
}

// Read config.json from LittleFS and return as String; empty if not present
String readConfigFile() {
  if (!LittleFS.begin()) {
    Serial.println("LittleFS mount failed (read)");
    return "";
  }
  if (!LittleFS.exists("/config.json")) {
    Serial.println("No config.json found");
    return "";
  }
  File f = LittleFS.open("/config.json", FILE_READ);
  if (!f) {
    Serial.println("Failed to open config.json");
    return "";
  }
  String s;
  while (f.available()) s += (char)f.read();
  f.close();
  return s;
}

// Small integer fallback channel for JSON parsing and runtime assignment.
// We keep the library's `MIDI_CHANNEL` as-is (it is a Control Surface
// Channel constant), but use a plain int for JSON/default handling.
constexpr int DEFAULT_MIDI_CHANNEL = 3;

// Lightweight wrapper kept for compatibility: forward to the main constructor
void createControlsFromConfig(const String &json) {
  constructControlsFromConfig(json);
}

// Construct runtime control objects from JSON config (or defaults)
void constructControlsFromConfig(const String &json) {
  // Free any existing objects
  for (int i = 0; i < 7; ++i) {
    if (buttons[i]) { delete buttons[i]; buttons[i] = nullptr; }
  }
  for (int i = 0; i < 6; ++i) {
    if (pots[i]) { delete pots[i]; pots[i] = nullptr; }
  }

  // Default channel
  int defaultChannel = DEFAULT_MIDI_CHANNEL;

  // Parse JSON if present
  DynamicJsonDocument doc(8192);
  if (json.length() > 0) {
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
      Serial.print("JSON parse error: "); Serial.println(err.c_str());
    }
  }

  // Build buttons
  for (int i = 0; i < 7; ++i) {
    int pin = BUTTON_PINS[i];
    int note = DEFAULT_BUTTON_NOTES[i];
    int channel = defaultChannel;
    // If config exists for button_i use its channel/value
    char idbuf[16];
    sprintf(idbuf, "button_%d", i+1);
    if (doc.containsKey(idbuf)) {
      JsonObject obj = doc[idbuf];
      if (obj.containsKey("channel")) channel = obj["channel"].as<int>();
      if (obj.containsKey("note")) note = obj["note"].as<int>();
    }
  // Construct a MIDIAddress (note + channel) for the NoteButton
  cs::MIDIAddress addr_note = cs::MIDIAddress(note, static_cast<cs::Channel>(channel));
  buttons[i] = new NoteButton(pin, addr_note);
  }

  // Build pots
  for (int i = 0; i < 6; ++i) {
    int index = i+1; // Control Surface pot indices are 1-based
    int cc = DEFAULT_POT_CCS[i];
    int channel = defaultChannel;
    char idbuf[16];
    sprintf(idbuf, "knob_%d", index);
    if (doc.containsKey(idbuf)) {
      JsonObject obj = doc[idbuf];
      if (obj.containsKey("channel")) channel = obj["channel"].as<int>();
      if (obj.containsKey("cc")) cc = obj["cc"].as<int>();
    }
  // Construct a MIDIAddress (CC + channel) for the CCPotentiometer
  cs::MIDIAddress addr_cc = cs::MIDIAddress(cc, static_cast<cs::Channel>(channel));
  pots[i] = new CCPotentiometer(index, addr_cc);
  }

  Serial.println("Controls constructed from config/defaults");
}

// Try to read SysEx from available MIDI sources
void pollSysEx() {
  // Try raw usbMIDI if available (Arduino core)
#if defined(usbMIDI)
  while (usbMIDI.read()) {
    if (usbMIDI.getType() == usbMIDI.SysEx) {
      // usbMIDI packages full SysEx messages; getSysEx is not universally available,
      // so collect bytes via getSysExData if present. Fallback: use getSysEx toString.
      auto len = usbMIDI.getSysExSize();
      sysexBuffer = "";
      for (size_t i = 0; i < len; ++i) {
        int b = usbMIDI.getSysExData(i);
        sysexBuffer += (char)b;
      }
      applyConfigJson(sysexBuffer);
    }
  }
#endif

  // Fallback: if Control Surface exposes a MIDI input pipe, it could be used here.
  // As a universal fallback we also support Serial-line JSON input handled in loop().
}

void setup() {
  // Start Serial first so wakeup diagnostics are visible immediately
  Serial.begin(115200);
  delay(50);
  Serial.println("--- boot ---");
  // Handle wake up from deep sleep (prints will now be visible)
  handleWakeup();
  // Read persisted config and construct controls
  String cfg = readConfigFile();
  constructControlsFromConfig(cfg);
  Control_Surface | pipes | midi;
  Control_Surface | pipes | bmidi;
  Control_Surface | pipes | serialmidi;


  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);  // GRB ordering is typical
  Control_Surface.begin(); 
  
  // Initialize activity time
  lastActivityTime = millis();

  // Clear any LEDs that might have been on before sleep
  setAllLEDs(CRGB::Black);

  // Touch pots array to ensure they are linked by the Control Surface library
  for (auto &p : pots) {
    // no-op: referencing the object ensures it is not optimized away and allows
    // library registration in contexts where explicit wiring is needed.
    (void)p;
  }
}

void loop() {
  Control_Surface.loop(); 
  // Handle button LED updates and activity by iterating the buttons array
  for (int i = 0; i < 7; ++i) {
    if (buttons[i]) handleButtonLED(*buttons[i]);
  }
  


  // Check for inactivity
  if (millis() - lastActivityTime > sleepTimeout) {
    goToSleep();
  }

  // Poll for incoming SysEx via MIDI
  pollSysEx();

  // Serial fallback: read newline-terminated JSON config
  static String serialLine = "";
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      if (serialLine.length() > 0) {
        applyConfigJson(serialLine);
        serialLine = "";
      }
    } else {
      serialLine += c;
      // guard against runaway lines
      if (serialLine.length() > 8192) serialLine = serialLine.substring(serialLine.length() - 8192);
    }
  }
}