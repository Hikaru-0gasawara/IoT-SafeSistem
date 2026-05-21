/**
 * IoT-SafeSistem — Electronic Safe for ESP32
 *
 * Real-hardware build using ESP32 DevKit v1, I2C 16x2 LCD (PCF8574 backpack),
 * 3x4 membrane keypad and SG90 micro servo.
 *
 * Adapted from Uri Shaked's Arduino Electronic Safe (MIT License, 2020).
 *
 * Servo behaviour:
 *   - Correct code typed   -> servo rotates to UNLOCK position
 *   - Cabinet locked       -> servo rotates to LOCK position
 *
 * !!! IMPORTANT — servo power supply !!!
 *   Do NOT power the SG90 from the ESP32's 5V/VIN rail on real hardware.
 *   Stall current (~600 mA) will brown-out the ESP32. Use an external 5V
 *   supply (>=1 A) and tie its ground to the ESP32 GND:
 *
 *       External 5V (+) --> Servo V+   (red)
 *       External 5V (-) --> Servo GND  (brown) --> ESP32 GND
 *       ESP32 GPIO18    --> Servo PWM  (orange)
 *
 * Wiring summary:
 *   LCD VCC -> 5V              Keypad R1..R4 -> GPIO 13, 14, 27, 26
 *   LCD GND -> GND             Keypad C1..C3 -> GPIO 25, 33, 32
 *   LCD SDA -> GPIO 21
 *   LCD SCL -> GPIO 22
 *
 * Required libraries (Arduino IDE -> Library Manager):
 *   - LiquidCrystal_I2C  (any fork exposing begin(cols, rows))
 *   - Keypad             by Mark Stanley / Alexander Brevig
 *   - ESP32Servo         by Kevin Harrington
 *
 * Board: Tools -> Board -> ESP32 Dev Module
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Keypad.h>
#include <ESP32Servo.h>
#include <Preferences.h>


// =====================================================================
//   Configuration constants
// =====================================================================

// ----- Servo --------------------------------------------------------
#define SERVO_PIN              18
#define SERVO_LOCK_POS         0     // adjust to your mechanism
#define SERVO_UNLOCK_POS       90    // swap with LOCK_POS if the lock opens the other way
#define SERVO_STEP_DELAY_MS    8     // ms per degree (smaller = faster)
#define SERVO_SETTLE_DELAY_MS  150   // settle time before detach
#define SERVO_PULSE_MIN_US     500
#define SERVO_PULSE_MAX_US     2400

// ----- LCD ----------------------------------------------------------
// If 0x27 doesn't respond, try 0x3F. Uncomment the I2C scanner at the
// bottom of this file to discover the actual address.
#define LCD_ADDR  0x27
#define LCD_COLS  16
#define LCD_ROWS  2

// ----- Keypad -------------------------------------------------------
#define KEYPAD_ROWS  4
#define KEYPAD_COLS  3

// ----- Code & UI ----------------------------------------------------
#define CODE_LENGTH    4    // display strings ([____]) assume length 4
#define KEY_LOCK       '#'  // press to lock the safe
#define KEY_NEW_CODE   '*'  // press to set/change the code
#define KEY_POLL_MS    10   // delay between keypad polls (keeps watchdog happy)


// =====================================================================
//   Hardware bindings
// =====================================================================

byte rowPins[KEYPAD_ROWS] = {13, 14, 27, 26};
byte colPins[KEYPAD_COLS] = {25, 33, 32};
char keys[KEYPAD_ROWS][KEYPAD_COLS] = {
  {'1', '2', '3'},
  {'4', '5', '6'},
  {'7', '8', '9'},
  {'*', '0', '#'}
};

LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, KEYPAD_ROWS, KEYPAD_COLS);
Servo lockServo;

// Tracks the last commanded angle so smooth moves start from the right spot.
int currentServoPos = SERVO_LOCK_POS;


// =====================================================================
//   Custom LCD glyphs
// =====================================================================

#define ICON_LOCKED_CHAR    (byte)0
#define ICON_UNLOCKED_CHAR  (byte)1
#define ICON_RIGHT_ARROW    (byte)126   // built-in HD44780 A00 right arrow

byte iconLocked[8] = {
  0b01110, 0b10001, 0b10001, 0b11111,
  0b11011, 0b11011, 0b11111, 0b00000,
};

byte iconUnlocked[8] = {
  0b01110, 0b10000, 0b10000, 0b11111,
  0b11011, 0b11011, 0b11111, 0b00000,
};


// =====================================================================
//   SafeState — persistence on ESP32 NVS (Preferences)
//
//   Preferences are encapsulated inside the class so that no other
//   module accidentally writes to the "safe" namespace.
// =====================================================================

class SafeState {
  public:
    void begin() {
      _prefs.begin("safe", false);
      _locked = _prefs.getBool("locked", false);
    }

    void lock()    { setLock(true); }
    bool locked()  { return _locked; }
    bool hasCode() { return _prefs.isKey("code"); }

    void setCode(const String &c) { _prefs.putString("code", c); }

    // Returns true and persists unlocked state on success; false otherwise.
    // If NVS lost the "code" key (e.g. partial wipe) we fail-open as a
    // recovery path — otherwise the device would be permanently bricked.
    bool unlock(const String &code) {
      if (!_prefs.isKey("code")) {
        setLock(false);
        return true;
      }
      if (_prefs.getString("code") != code) return false;
      setLock(false);
      return true;
    }

  private:
    void setLock(bool l) {
      _locked = l;
      _prefs.putBool("locked", l);
    }

    Preferences _prefs;
    bool        _locked = false;
};

SafeState safeState;


// =====================================================================
//   Servo primitives
//
//   moveServoSmooth() ramps from currentServoPos to target one degree
//   at a time, then detaches the servo to silence the SG90 buzz/jitter.
//   The mechanical lock holds position once the PWM is removed.
// =====================================================================

void moveServoSmooth(int target) {
  if (!lockServo.attached()) {
    lockServo.setPeriodHertz(50);
    lockServo.attach(SERVO_PIN, SERVO_PULSE_MIN_US, SERVO_PULSE_MAX_US);
  }

  int step = (target > currentServoPos) ? 1 : -1;
  while (currentServoPos != target) {
    currentServoPos += step;
    lockServo.write(currentServoPos);
    delay(SERVO_STEP_DELAY_MS);
  }

  delay(SERVO_SETTLE_DELAY_MS);
  lockServo.detach();
}

// Renamed from lock()/unlock() to avoid colliding with SafeState::lock()/unlock().
void engageLock() {
  Serial.println(F("[SERVO] LOCK"));
  moveServoSmooth(SERVO_LOCK_POS);
  safeState.lock();
}

void releaseLock() {
  Serial.println(F("[SERVO] UNLOCK"));
  moveServoSmooth(SERVO_UNLOCK_POS);
}


// =====================================================================
//   Input helpers
// =====================================================================

// Blocks until a key is pressed, yielding to the scheduler between polls.
char waitForKey() {
  while (true) {
    char k = keypad.getKey();
    if (k != NO_KEY) return k;
    delay(KEY_POLL_MS);
  }
}

// Waits until the user presses one of the chars listed in `accepted`.
char waitForKeyIn(const char *accepted) {
  while (true) {
    char k = keypad.getKey();
    if (k != NO_KEY) {
      for (const char *p = accepted; *p; p++) {
        if (*p == k) return k;
      }
    }
    delay(KEY_POLL_MS);
  }
}

String inputSecretCode() {
  lcd.setCursor(5, 1);
  lcd.print('[');
  for (byte i = 0; i < CODE_LENGTH; i++) lcd.print('_');
  lcd.print(']');
  lcd.setCursor(6, 1);

  String result;
  result.reserve(CODE_LENGTH);
  while (result.length() < CODE_LENGTH) {
    char key = waitForKey();
    if (key >= '0' && key <= '9') {
      lcd.print('*');
      result += key;
    }
  }
  return result;
}

void showWaitScreen(int stepMs) {
  lcd.setCursor(2, 1);
  lcd.print("[..........]");
  lcd.setCursor(3, 1);
  for (byte i = 0; i < 10; i++) {
    delay(stepMs);
    lcd.print('=');
  }
}


// =====================================================================
//   Screens
// =====================================================================

void showStartupMessage() {
  lcd.setCursor(4, 0);
  lcd.print(F("Welcome!"));
  delay(1000);

  lcd.setCursor(0, 1);
  const char *msg = "ESP32 Safe v1.0";
  for (const char *p = msg; *p; p++) {
    lcd.print(*p);
    delay(100);
  }
  delay(500);
}

void showUnlockMessage() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.write(ICON_UNLOCKED_CHAR);
  lcd.setCursor(4, 0);
  lcd.print(F("Unlocked!"));
  lcd.setCursor(15, 0);
  lcd.write(ICON_UNLOCKED_CHAR);
  delay(1000);
}

bool setNewCode() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Enter new code:"));
  String newCode = inputSecretCode();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(F("Confirm new code"));
  String confirmCode = inputSecretCode();

  if (newCode.equals(confirmCode)) {
    safeState.setCode(newCode);
    return true;
  }

  lcd.clear();
  lcd.setCursor(1, 0);
  lcd.print(F("Code mismatch"));
  lcd.setCursor(0, 1);
  lcd.print(F("Safe not locked!"));
  delay(2000);
  return false;
}


// =====================================================================
//   State machines
// =====================================================================

void safeUnlockedLogic() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.write(ICON_UNLOCKED_CHAR);
  lcd.setCursor(2, 0);
  lcd.print(F(" # to lock"));
  lcd.setCursor(15, 0);
  lcd.write(ICON_UNLOCKED_CHAR);

  const bool newCodeNeeded = !safeState.hasCode();
  if (!newCodeNeeded) {
    lcd.setCursor(0, 1);
    lcd.print(F("  * = new code"));
  }

  // Wait for either KEY_NEW_CODE or KEY_LOCK without busy-spinning.
  char key;
  if (newCodeNeeded) {
    // No code stored — only KEY_LOCK exits the loop but we always
    // force a setNewCode() before locking anyway.
    key = waitForKeyIn("*#");
  } else {
    key = waitForKeyIn("*#");
  }

  bool readyToLock = true;
  if (key == KEY_NEW_CODE || newCodeNeeded) {
    readyToLock = setNewCode();
  }

  if (readyToLock) {
    lcd.clear();
    lcd.setCursor(5, 0);
    lcd.write(ICON_UNLOCKED_CHAR);
    lcd.print(' ');
    lcd.write(ICON_RIGHT_ARROW);
    lcd.print(' ');
    lcd.write(ICON_LOCKED_CHAR);

    engageLock();
    showWaitScreen(100);
  }
}

void safeLockedLogic() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.write(ICON_LOCKED_CHAR);
  lcd.print(F(" Safe Locked! "));
  lcd.write(ICON_LOCKED_CHAR);

  String userCode = inputSecretCode();
  bool unlockedOk = safeState.unlock(userCode);
  showWaitScreen(200);

  if (unlockedOk) {
    showUnlockMessage();
    releaseLock();
  } else {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(F("Access Denied!"));
    showWaitScreen(1000);
  }
}


// =====================================================================
//   setup / loop
// =====================================================================

void setup() {
  Serial.begin(115200);
  delay(200);   // let the PSU stabilise before driving the servo

  Wire.begin();           // SDA=21, SCL=22 (ESP32 defaults)
  lcd.begin(LCD_COLS, LCD_ROWS);
  lcd.backlight();
  lcd.createChar(ICON_LOCKED_CHAR,   iconLocked);
  lcd.createChar(ICON_UNLOCKED_CHAR, iconUnlocked);

  lockServo.setPeriodHertz(50);
  lockServo.attach(SERVO_PIN, SERVO_PULSE_MIN_US, SERVO_PULSE_MAX_US);

  safeState.begin();

  // Slam the servo to the persisted position without animation — we don't
  // know where the shaft actually is after a power-on/reset.
  const int initialPos = safeState.locked() ? SERVO_LOCK_POS : SERVO_UNLOCK_POS;
  lockServo.write(initialPos);
  currentServoPos = initialPos;
  delay(400);
  lockServo.detach();

  showStartupMessage();
}

void loop() {
  if (safeState.locked()) {
    safeLockedLogic();
  } else {
    safeUnlockedLogic();
  }
}


// =====================================================================
//   Optional: I2C scanner — paste into setup() if the LCD stays blank.
// =====================================================================
//
//   Wire.begin();
//   delay(100);
//   for (byte a = 1; a < 127; a++) {
//     Wire.beginTransmission(a);
//     if (Wire.endTransmission() == 0) {
//       Serial.printf("I2C device at 0x%02X\n", a);
//     }
//   }
