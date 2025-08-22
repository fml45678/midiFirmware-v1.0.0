  /** 
 * Written by PieterP, 2019-08-07  
 * https://github.com/tttapa/Control-Surface
 */
#include <FastLED.h>
constexpr int NUM_LEDS = 2;
constexpr int DATA_PIN = 13;
CRGB leds[NUM_LEDS];

// Uncomment to disable actual deep sleep for debugging wake logic (keeps Serial alive)
#define DEBUG_NO_SLEEP

#include <Control_Surface.h> // Include the Control Surface library
#include <esp_sleep.h>
#include <esp_wifi.h>
#include <esp_bt.h>
#include <driver/rtc_io.h>
// Filesystem for storing config
#include <LittleFS.h>
#include <FS.h>
#include <SPIFFS.h>
// JSON parsing
#include <ArduinoJson.h>
#include <Preferences.h>

// Centralized MIDI channel used by buttons and pots. Change here to reassign all controls.
constexpr auto MIDI_CHANNEL = CHANNEL_3;

// Board-specific counts — keep these in sync with BUTTON_PINS / any pot wiring
constexpr int BUTTON_COUNT = 2;
constexpr int POT_COUNT = 2;
// If you wire pots to specific ADC pins and need to document them, update here.
constexpr int POT_PINS[] = {1, 2};

// Instantiate a MIDI over USB interface or Bluetooth.
USBMIDI_Interface midi;
BluetoothMIDI_Interface bmidi;
HardwareSerialMIDI_Interface serialmidi {Serial1, MIDI_BAUD};

BidirectionalMIDI_PipeFactory<3> pipes;

// Inactivity timer
unsigned long lastActivityTime = 0;
constexpr unsigned long sleepTimeout = 120000; // 2 minutes in ms

// Runtime override to prevent deep sleep (useful if compile-time DEBUG_NO_SLEEP
// wasn't applied or you want to toggle at runtime). Set to `true` to keep the
// device awake while debugging.
bool FORCE_NO_SLEEP = true;


// Button and wake pin definitions
// Hardware pin mapping
constexpr int BUTTON_PINS[] = {3,4};
// Wake pins to use for EXT1 wake on this board (Seeed XIAO S3) - user requested
constexpr int WAKE_PINS[] = {3,4};

// Runtime control containers (constructed in setup() from config or defaults)
NoteButton* buttons[BUTTON_COUNT] = {nullptr};
CCPotentiometer* pots[POT_COUNT] = {nullptr};

// Default mapping data (used if no config present)
constexpr int DEFAULT_BUTTON_NOTES[2] = {
  MIDI_Notes::C(4), MIDI_Notes::D(4)
};
constexpr int DEFAULT_POT_CCS[2] = {
  MIDI_CC::Channel_Volume, MIDI_CC::Pan
};

// Per-control default channels (user-facing 1..16). Keep in sync with counts.
constexpr int DEFAULT_BUTTON_CHANNELS[BUTTON_COUNT] = { DEFAULT_MIDI_CHANNEL, DEFAULT_MIDI_CHANNEL };
constexpr int DEFAULT_POT_CHANNELS[POT_COUNT] = { DEFAULT_MIDI_CHANNEL, DEFAULT_MIDI_CHANNEL };

// Compile-time checks to ensure the pin arrays match the counts
static_assert((int)(sizeof(BUTTON_PINS)/sizeof(BUTTON_PINS[0])) == BUTTON_COUNT, "BUTTON_PINS size must equal BUTTON_COUNT");
static_assert((int)(sizeof(POT_PINS)/sizeof(POT_PINS[0])) == POT_COUNT, "POT_PINS size must equal POT_COUNT");
static_assert((int)(sizeof(DEFAULT_BUTTON_NOTES)/sizeof(DEFAULT_BUTTON_NOTES[0])) == BUTTON_COUNT, "DEFAULT_BUTTON_NOTES size must equal BUTTON_COUNT");
static_assert((int)(sizeof(DEFAULT_POT_CCS)/sizeof(DEFAULT_POT_CCS[0])) == POT_COUNT, "DEFAULT_POT_CCS size must equal POT_COUNT");

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
  // Runtime override: if FORCE_NO_SLEEP is true, skip entering deep sleep.
  if (FORCE_NO_SLEEP) {
    Serial.println("[FORCE_NO_SLEEP] Runtime override active — skipping deep sleep");
    // Mirror to Serial1 if available for hardware UART debugging
    Serial1.println("[FORCE_NO_SLEEP] Runtime override active — skipping deep sleep");
    return;
  }
  
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

// Filesystem tracking
enum class FSKind { None, LittleFS, SPIFFS } fsKind = FSKind::None;
bool fsMounted = false;
Preferences prefs;

void applyConfigJson(const String &json) {
  Serial.print("Received config JSON length: "); Serial.println(json.length());
  // Persist: prefer filesystem, fallback to Preferences
  if (fsMounted) {
    File f;
    if (fsKind == FSKind::LittleFS) {
      f = LittleFS.open("/config.json", FILE_WRITE);
    } else if (fsKind == FSKind::SPIFFS) {
      f = SPIFFS.open("/config.json", FILE_WRITE);
    }
    if (f) {
      f.print(json);
      f.close();
      Serial.println("Config written to /config.json");
    } else {
      Serial.println("Failed to open config file for writing");
    }
  } else {
    // Preferences fallback
    if (prefs.isKey("cfg")) prefs.remove("cfg");
    prefs.putString("cfg", json);
    Serial.println("Config saved to Preferences");
  }
  // Apply immediately
  constructControlsFromConfig(json);

  // Send a simple SysEx ACK back: [F0 7D 02 01 F7]
  // Attempt to send a SysEx ACK on the configured MIDI interface so the
  // web uploader can detect success. Control Surface's `midi` pipe is used
  // (works for USBMIDI/BluetoothMIDI when registered in Control_Surface).
  // Control Surface expects SysEx message payloads without the 0xF0/0xF7
  // framing bytes. Construct a SysExMessage (manufacturer + payload) and
  // send it via the `midi` interface.
  // Build a framed SysEx message for direct usbMIDI send (0xF0 ... 0xF7)
  byte ack[] = {0xF0, 0x7D, 0x02, 0x01, 0xF7};
#if defined(usbMIDI)
  // Use the low-level usbMIDI object to send raw SysEx bytes when available.
  usbMIDI.sendSysEx(sizeof(ack), ack);
  Serial.println("Sent SysEx ACK (usbMIDI)");
#else
  Serial.println("usbMIDI not available; SysEx ACK not sent");
#endif
  // Serial ack for upper-layer clients (web UI) to detect success
  Serial.println("CONFIG_APPLIED");
  // Visual debug: green pulse to indicate config applied
  setAllLEDs(CRGB::Green);
  delay(150);
  setAllLEDs(CRGB::Black);
}

// Read config.json from LittleFS and return as String; empty if not present
String readConfigFile() {
  if (!fsMounted) {
    Serial.println("No filesystem mounted (read)");
    // Fallback: return value from Preferences if present
    if (prefs.isKey("cfg")) {
      String s = prefs.getString("cfg", "");
      Serial.println("Loaded config from Preferences");
      return s;
    }
    return "";
  }
  bool exists = false;
  File f;
  if (fsKind == FSKind::LittleFS) {
    exists = LittleFS.exists("/config.json");
    if (!exists) { Serial.println("No config.json found (LittleFS)"); return ""; }
    f = LittleFS.open("/config.json", FILE_READ);
  } else if (fsKind == FSKind::SPIFFS) {
    exists = SPIFFS.exists("/config.json");
    if (!exists) { Serial.println("No config.json found (SPIFFS)"); return ""; }
    f = SPIFFS.open("/config.json", FILE_READ);
  }
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
  for (int i = 0; i < BUTTON_COUNT; ++i) {
    if (buttons[i]) { delete buttons[i]; buttons[i] = nullptr; }
  }
  for (int i = 0; i < POT_COUNT; ++i) {
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
  for (int i = 0; i < BUTTON_COUNT; ++i) {
    int pin = BUTTON_PINS[i];
    int note = DEFAULT_BUTTON_NOTES[i];
  int channel = DEFAULT_BUTTON_CHANNELS[i];
    // If config exists for button_i use its channel/value
    char idbuf[16];
    sprintf(idbuf, "button_%d", i+1);
    if (doc.containsKey(idbuf)) {
      JsonObject obj = doc[idbuf];
      if (obj.containsKey("channel")) channel = obj["channel"].as<int>();
      if (obj.containsKey("note")) note = obj["note"].as<int>();
    }
    // Clamp to 1..16 and convert to Control Surface's 0-based cs::Channel
    if (channel < 1) channel = 1;
    if (channel > 16) channel = 16;
    int csChannel = channel - 1;
  // Construct a MIDIAddress (note + channel) for the NoteButton
  cs::MIDIAddress addr_note = cs::MIDIAddress(note, static_cast<cs::Channel>(csChannel));
  buttons[i] = new NoteButton(pin, addr_note);
  // Debug print
  Serial.print("Constructed NoteButton pin="); Serial.print(pin);
  Serial.print(" note="); Serial.print(note);
  Serial.print(" channel="); Serial.println(channel);
  }

  // Build pots
  for (int i = 0; i < POT_COUNT; ++i) {
    int index = i+1; // Control Surface pot indices are 1-based
    int cc = DEFAULT_POT_CCS[i];
  int channel = DEFAULT_POT_CHANNELS[i];
    char idbuf[16];
    sprintf(idbuf, "knob_%d", index);
    if (doc.containsKey(idbuf)) {
      JsonObject obj = doc[idbuf];
      if (obj.containsKey("channel")) channel = obj["channel"].as<int>();
      if (obj.containsKey("cc")) cc = obj["cc"].as<int>();
    }
    // Clamp to 1..16 and convert to Control Surface's 0-based cs::Channel
    if (channel < 1) channel = 1;
    if (channel > 16) channel = 16;
    int csChannel = channel - 1;
  // Construct a MIDIAddress (CC + channel) for the CCPotentiometer
  cs::MIDIAddress addr_cc = cs::MIDIAddress(cc, static_cast<cs::Channel>(csChannel));
  pots[i] = new CCPotentiometer(index, addr_cc);
  // Debug print
  Serial.print("Constructed CCPotentiometer index="); Serial.print(index);
  Serial.print(" cc="); Serial.print(cc);
  Serial.print(" channel="); Serial.println(channel);
  }

  Serial.println("Controls constructed from config/defaults");
}

// Try to read SysEx from available MIDI sources
void pollSysEx() {
  // Try raw usbMIDI if available (Arduino core)
#if defined(usbMIDI)
  while (usbMIDI.read()) {
    // Log the incoming message type for debugging
    auto t = usbMIDI.getType();
    Serial.print("usbMIDI msg type: "); Serial.println((int)t);
    // If SysEx, extract and apply
    if (t == usbMIDI.SysEx) {
      Serial.println("usbMIDI SysEx received");
      // usbMIDI packages full SysEx messages; getSysEx is not universally available,
      // so collect bytes via getSysExData if present. Fallback: use getSysEx toString.
      auto len = usbMIDI.getSysExSize();
      sysexBuffer = "";
      Serial.print("SysEx size: "); Serial.println((unsigned long)len);
      for (size_t i = 0; i < len; ++i) {
        int b = usbMIDI.getSysExData(i);
        Serial.print(" 0x"); Serial.print(b, HEX);
        sysexBuffer += (char)b;
      }
      Serial.println();
      Serial.print("Collected SysEx payload len: "); Serial.println(sysexBuffer.length());
      // If this is a request for config (manufacturer 0x7D, cmd 0x03)
      if (sysexBuffer.length() >= 2 && (uint8_t)sysexBuffer[0] == 0x7D && (uint8_t)sysexBuffer[1] == 0x03) {
        Serial.println("Config request SysEx received — sending config reply");
        // Read stored config (prefer FS, fallback to Preferences)
        String cfg = readConfigFile();
        if (cfg.length() == 0 && prefs.isKey("cfg")) cfg = prefs.getString("cfg", "");
        if (cfg.length() == 0) {
          Serial.println("No config stored — sending empty JSON {}");
          cfg = "{}";
        }
        // Build a framed SysEx: F0 7D 04 <payload bytes...> F7 (0x04 = config reply)
        size_t payloadLen = cfg.length();
        size_t totalLen = payloadLen + 5; // F0 7D 04 ... F7
        uint8_t *buf = (uint8_t *)malloc(totalLen);
        if (buf) {
          buf[0] = 0xF0;
          buf[1] = 0x7D;
          buf[2] = 0x04;
          for (size_t j = 0; j < payloadLen; ++j) buf[3 + j] = (uint8_t)cfg[j];
          buf[3 + payloadLen] = 0xF7;
#if defined(usbMIDI)
#if defined(usbMIDI)
          usbMIDI.sendSysEx(totalLen, buf);
          Serial.println("Sent SysEx config reply (usbMIDI)");
          // Small delay to give the host a chance to receive/dispatch the SysEx
          delay(60);
          // Send a short Control Change as a presence ping so browsers that
          // don't deliver SysEx reliably still see device activity. Use CC#5
          // on channel 1 (Control Surface channels are 0-based here).
          usbMIDI.sendControlChange(5, 127, 0);
          Serial.println("Sent CC presence ping (usbMIDI)");
#else
          Serial.println("usbMIDI not available; printing config to Serial instead:");
          Serial.println(cfg);
#endif
          free(buf);
        } else {
          Serial.println("Memory allocation failed; cannot send config reply");
        }
      } else {
        // Otherwise treat incoming SysEx as a config JSON payload to apply
        applyConfigJson(sysexBuffer);
      }
  // Visual debug: Purple pulse to indicate SysEx arrived
  setAllLEDs(CRGB::Purple);
  delay(80);
  setAllLEDs(CRGB::Black);
    } else {
      // For any other MIDI message, print raw bytes where possible
      Serial.print("usbMIDI non-SysEx message. Raw: ");
      // usbMIDI doesn't expose a direct buffer API for channel messages, so
      // attempt to print status and available parameters.
      Serial.print("type="); Serial.print((int)t); Serial.print(" ");
      if (usbMIDI.getType() == usbMIDI.NoteOn || usbMIDI.getType() == usbMIDI.NoteOff) {
        Serial.print("note="); Serial.print(usbMIDI.getData1());
        Serial.print(" vel="); Serial.print(usbMIDI.getData2());
      } else if (usbMIDI.getType() == usbMIDI.ControlChange) {
        Serial.print("cc="); Serial.print(usbMIDI.getData1());
        Serial.print(" val="); Serial.print(usbMIDI.getData2());
      }
      Serial.println();
  // Visual debug: Purple pulse to indicate MIDI activity arrived
  setAllLEDs(CRGB::Purple);
  delay(80);
  setAllLEDs(CRGB::Black);
    }
  }
#endif

  // Fallback: if Control Surface exposes a MIDI input pipe, it could be used here.
  // As a universal fallback we also support Serial-line JSON input handled in loop().
}

void setup() {
  // Start Serial first so wakeup diagnostics are visible immediately
  Serial.begin(115200);
  delay(2000);
  Serial.println("--- boot --- (DEBUG_NO_SLEEP)");
  // Handle wake up from deep sleep (prints will now be visible)
  handleWakeup();
  // Attempt to mount a filesystem for config persistence
  Serial.println("Mounting filesystem...");
  if (LittleFS.begin()) {
    fsKind = FSKind::LittleFS;
    fsMounted = true;
    Serial.println("Mounted LittleFS");
  } else if (SPIFFS.begin()) {
    fsKind = FSKind::SPIFFS;
    fsMounted = true;
    Serial.println("Mounted SPIFFS");
  } else {
    fsKind = FSKind::None;
    fsMounted = false;
    Serial.println("No filesystem mounted");
  }
  // Initialize Preferences as a fallback storage
  prefs.begin("madMidi", false);
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
#ifdef DEBUG_NO_SLEEP
  static unsigned long hb_last = 0;
  if (millis() - hb_last > 1000) {
    hb_last = millis();
    Serial.println("[DEBUG HEARTBEAT]");
  }
#endif
  Control_Surface.loop(); 
  // Handle button LED updates and activity by iterating the buttons array
  for (int i = 0; i < BUTTON_COUNT; ++i) {
    if (buttons[i]) handleButtonLED(*buttons[i]);
  }
  


  // Check for inactivity
  if (millis() - lastActivityTime > sleepTimeout) {
    goToSleep();
  }

  // Poll for incoming SysEx via MIDI
  pollSysEx();

  // Serial fallback: read newline-terminated JSON config (also accept complete
  // JSON objects by tracking brace depth so a trailing newline is not required)
  static String serialLine = "";
  static int braceDepth = 0;
  while (Serial.available()) {
    char c = (char)Serial.read();
    // Verbose debug: show exact bytes arriving so we can diagnose line endings / encoding
    Serial.print("RX char: "); Serial.print((int)c); Serial.print(" ");
    if (isPrintable(c)) {
      Serial.print("'"); Serial.print(c); Serial.print("'");
    } else {
      Serial.print("(non-printable)");
    }
    Serial.println();

    // Update brace depth for flow that sends raw JSON without newline
    if (c == '{') ++braceDepth;
    else if (c == '}') --braceDepth;

    // Accept either LF or CR as a line terminator (handle CRLF or LF-only)
    if (c == '\n' || c == '\r') {
      if (serialLine.length() > 0) {
        Serial.print("Serial line received (<len="); Serial.print(serialLine.length()); Serial.print("): ");
        Serial.println(serialLine);
        applyConfigJson(serialLine);
        serialLine = "";
        braceDepth = 0;
      } else {
        // ignore empty line (possible extra CR/LF)
      }
    } else {
      serialLine += c;
      // guard against runaway lines
      if (serialLine.length() > 8192) serialLine = serialLine.substring(serialLine.length() - 8192);
      // If we just closed the top-level JSON object, accept it immediately
      if (braceDepth <= 0 && serialLine.length() > 0) {
        Serial.print("Serial JSON object complete (braceDepth<=0) len="); Serial.println(serialLine.length());
        Serial.print("Serial line received: "); Serial.println(serialLine);
        applyConfigJson(serialLine);
        serialLine = "";
        braceDepth = 0;
      }
    }
  }
}