"""
Arduino Electronic Safe — port to MicroPython (Raspberry Pi Pico).

Based on the original project by Uri Shaked (2020, MIT License).

Hardware pin mapping (Pico GP numbers):
  LCD        : RS=12, EN=11, D4=10, D5=9, D6=8, D7=7
  Keypad rows: GP5, GP4, GP3, GP2   (R1..R4)
  Keypad cols: GP13, GP14, GP15, GP16  (C1..C4)
  Servo      : GP6

Note: diagram.json shows the equivalent Arduino Uno circuit (same pin numbers
for LCD/keypad rows/servo; Uno analog pins A0-A3 replace GP13-GP16 for cols).
"""

from machine import Pin, PWM
import time
import os


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
CODE_LENGTH      = 4
SERVO_PIN        = 6
SERVO_LOCK_POS   = 20
SERVO_UNLOCK_POS = 90


# ---------------------------------------------------------------------------
# LCD 1602  (HD44780, 4-bit mode, custom-character support)
# ---------------------------------------------------------------------------
class LCD1602:
    def __init__(self, rs, en, d4, d5, d6, d7, cols=16, rows=2):
        self.rs   = Pin(rs, Pin.OUT)
        self.en   = Pin(en, Pin.OUT)
        self.data = [Pin(p, Pin.OUT) for p in (d4, d5, d6, d7)]
        self.cols = cols
        self.rows = rows
        self._init_lcd()

    # -- low-level ----------------------------------------------------------

    def _pulse(self):
        self.en.value(1)
        time.sleep_us(1)
        self.en.value(0)
        time.sleep_us(100)

    def _write4(self, nibble):
        for i in range(4):
            self.data[i].value((nibble >> i) & 1)
        self._pulse()

    def _send(self, byte, mode):
        self.rs.value(mode)
        self._write4(byte >> 4)
        self._write4(byte & 0x0F)

    def _cmd(self, byte):
        self._send(byte, 0)

    # -- initialisation  (HD44780 4-bit handshake) --------------------------

    def _init_lcd(self):
        time.sleep_ms(50)
        self.rs.value(0)
        self.en.value(0)
        # Three soft-reset pulses with decreasing delays: >4.1 ms, >100 µs, >100 µs
        for delay_ms in (5, 1, 1):
            self._write4(0x03)
            time.sleep_ms(delay_ms)
        self._write4(0x02)   # switch to 4-bit mode
        self._cmd(0x28)      # 4-bit, 2 lines, 5×8 font
        self._cmd(0x0C)      # display ON, cursor OFF, blink OFF
        self._cmd(0x06)      # auto-increment cursor, no display shift
        self.clear()

    # -- public API ---------------------------------------------------------

    def clear(self):
        self._cmd(0x01)
        time.sleep_ms(2)     # clear needs ≥1.52 ms

    def set_cursor(self, col, row):
        self._cmd(0x80 | (col + (0x40 if row else 0x00)))

    def print(self, text):
        for ch in str(text):
            self._send(ord(ch), 1)

    def write(self, byte):
        """Write a raw byte — used to display custom-char slots 0..7."""
        self._send(byte, 1)

    def create_char(self, location, charmap):
        """Store a 5×8 bitmap in one of the 8 CGRAM slots (location 0..7)."""
        location &= 0x07
        self._cmd(0x40 | (location << 3))
        for row in charmap:
            self._send(row, 1)
        self._cmd(0x80)      # return cursor to DDRAM address 0


# ---------------------------------------------------------------------------
# 4×4 matrix keypad
# ---------------------------------------------------------------------------
class Keypad4x4:
    KEYS = (
        ('1', '2', '3', 'A'),
        ('4', '5', '6', 'B'),
        ('7', '8', '9', 'C'),
        ('*', '0', '#', 'D'),
    )

    def __init__(self, row_pins, col_pins):
        # Rows: driven LOW one at a time; idle HIGH
        self.rows = [Pin(p, Pin.OUT, value=1) for p in row_pins]
        # Cols: input with pull-up; a pressed key pulls the column LOW
        self.cols = [Pin(p, Pin.IN, Pin.PULL_UP) for p in col_pins]
        self._last_key = None

    def _scan(self):
        for r, row_pin in enumerate(self.rows):
            row_pin.value(0)
            time.sleep_us(5)
            for c, col_pin in enumerate(self.cols):
                if col_pin.value() == 0:
                    row_pin.value(1)
                    return self.KEYS[r][c]
            row_pin.value(1)
        return None

    def get_key(self):
        """Return a key only on the press edge (one event per physical click)."""
        key = self._scan()
        if key != self._last_key:
            self._last_key = key
            if key is not None:
                time.sleep_ms(20)  # debounce
                return key
        return None


# ---------------------------------------------------------------------------
# Servo  (50 Hz PWM, 500–2500 µs pulse range)
# ---------------------------------------------------------------------------
class Servo:
    _MIN_US    = 500
    _MAX_US    = 2500
    _PERIOD_US = 20_000

    def __init__(self, pin):
        self.pwm = PWM(Pin(pin))
        self.pwm.freq(50)

    def write(self, angle):
        """Move to angle (0..180 degrees)."""
        angle     = max(0, min(180, angle))
        pulse_us  = self._MIN_US + (angle * (self._MAX_US - self._MIN_US)) // 180
        self.pwm.duty_u16(pulse_us * 65535 // self._PERIOD_US)

    def release(self):
        """Stop PWM pulses to prevent idle jitter."""
        self.pwm.duty_u16(0)


# ---------------------------------------------------------------------------
# SafeState — flash-backed persistence (replaces Arduino EEPROM)
#
# File "safe.dat" layout:
#   byte 0     : locked flag (0 = unlocked, 1 = locked)
#   byte 1     : code length (0..MAX_CODE_LENGTH)
#   bytes 2..N : code in ASCII
# ---------------------------------------------------------------------------
class SafeState:
    FILE            = "safe.dat"
    MAX_CODE_LENGTH = 16

    def __init__(self):
        try:
            os.stat(self.FILE)
        except OSError:
            self._write(bytearray([0, 0]))  # first boot: unlocked, no code

    def _read(self):
        try:
            with open(self.FILE, "rb") as f:
                data = bytearray(f.read())
            if len(data) >= 2:
                return data
        except OSError:
            pass
        # File missing or corrupt — reset to safe defaults
        default = bytearray([0, 0])
        self._write(default)
        return default

    def _write(self, data):
        with open(self.FILE, "wb") as f:
            f.write(bytes(data))

    def locked(self):
        return self._read()[0] != 0

    def has_code(self):
        return self._read()[1] > 0

    def set_code(self, new_code):
        code_bytes = new_code.encode()[:self.MAX_CODE_LENGTH]
        data = self._read()
        self._write(bytearray([data[0], len(code_bytes)]) + code_bytes)

    def lock(self):
        data = self._read()
        data[0] = 1
        self._write(data)

    def unlock(self, code):
        """Return True and persist unlocked state if code matches, else False."""
        data = self._read()
        code_len = data[1]
        if code_len == 0 or code_len != len(code):
            return False
        stored = bytes(data[2:2 + code_len])
        if stored != code.encode():
            return False
        data[0] = 0
        self._write(data)
        return True


# ---------------------------------------------------------------------------
# Custom LCD icons in CGRAM slots 0..2
# ---------------------------------------------------------------------------
ICON_UNLOCKED = 0
ICON_LOCKED   = 1
ICON_ARROW    = 2

_ICON_BITMAPS = {
    ICON_UNLOCKED: bytes((0x06, 0x09, 0x09, 0x08, 0x1F, 0x1B, 0x1B, 0x1F)),
    ICON_LOCKED:   bytes((0x0E, 0x11, 0x11, 0x1F, 0x1B, 0x1B, 0x1B, 0x1F)),
    ICON_ARROW:    bytes((0x00, 0x08, 0x0C, 0x0E, 0x0C, 0x08, 0x00, 0x00)),
}


def init_icons(lcd):
    for slot, bitmap in _ICON_BITMAPS.items():
        lcd.create_char(slot, bitmap)


# ---------------------------------------------------------------------------
# Hardware initialisation
# ---------------------------------------------------------------------------
lcd        = LCD1602(rs=12, en=11, d4=10, d5=9, d6=8, d7=7)
keypad     = Keypad4x4(row_pins=(5, 4, 3, 2), col_pins=(13, 14, 15, 16))
lock_servo = Servo(SERVO_PIN)
safe_state = SafeState()


# ---------------------------------------------------------------------------
# Physical lock helpers
# ---------------------------------------------------------------------------
def do_lock():
    lock_servo.write(SERVO_LOCK_POS)
    safe_state.lock()


def do_unlock():
    lock_servo.write(SERVO_UNLOCK_POS)


# ---------------------------------------------------------------------------
# UI helpers
# ---------------------------------------------------------------------------
def wait_for_key():
    """Block until a key is pressed; return the character."""
    while True:
        k = keypad.get_key()
        if k is not None:
            return k
        time.sleep_ms(10)


def show_startup_message():
    lcd.set_cursor(4, 0)
    lcd.print("Welcome!")
    time.sleep_ms(1000)
    lcd.set_cursor(0, 1)
    for ch in "ArduinoSafe v1.0":
        lcd.print(ch)
        time.sleep_ms(100)
    time.sleep_ms(500)


def input_secret_code():
    """Show masked input field and collect exactly CODE_LENGTH digits."""
    lcd.set_cursor(5, 1)
    lcd.print("[" + "_" * CODE_LENGTH + "]")
    lcd.set_cursor(6, 1)
    result = ""
    while len(result) < CODE_LENGTH:
        key = wait_for_key()
        if '0' <= key <= '9':
            lcd.print('*')
            result += key
    return result


def show_wait_screen(step_ms):
    """Animated progress bar that fills over 10 steps."""
    lcd.set_cursor(2, 1)
    lcd.print("[" + "." * 10 + "]")
    lcd.set_cursor(3, 1)
    for _ in range(10):
        time.sleep_ms(step_ms)
        lcd.print("=")


def show_unlock_message():
    lcd.clear()
    lcd.set_cursor(0, 0)
    lcd.write(ICON_UNLOCKED)
    lcd.set_cursor(4, 0)
    lcd.print("Unlocked!")
    lcd.set_cursor(15, 0)
    lcd.write(ICON_UNLOCKED)
    time.sleep_ms(1000)


def set_new_code():
    """Prompt for a new code with confirmation. Returns True on success."""
    lcd.clear()
    lcd.set_cursor(0, 0)
    lcd.print("Enter new code:")
    new_code = input_secret_code()

    lcd.clear()
    lcd.set_cursor(0, 0)
    lcd.print("Confirm new code")
    confirm = input_secret_code()

    if new_code == confirm:
        safe_state.set_code(new_code)
        return True

    lcd.clear()
    lcd.set_cursor(1, 0)
    lcd.print("Code mismatch!")
    lcd.set_cursor(0, 1)
    lcd.print("Safe not locked!")
    time.sleep_ms(2000)
    return False


# ---------------------------------------------------------------------------
# Main state machines
# ---------------------------------------------------------------------------
def safe_unlocked_logic():
    lcd.clear()
    lcd.set_cursor(0, 0)
    lcd.write(ICON_UNLOCKED)
    lcd.set_cursor(2, 0)
    lcd.print("# to lock")
    lcd.set_cursor(15, 0)
    lcd.write(ICON_UNLOCKED)

    need_new_code = not safe_state.has_code()
    if not need_new_code:
        lcd.set_cursor(0, 1)
        lcd.print("  A = new code")

    key = None
    while key not in ('A', '#'):
        key = keypad.get_key()
        time.sleep_ms(10)

    ready_to_lock = True
    if key == 'A' or need_new_code:
        ready_to_lock = set_new_code()

    if ready_to_lock:
        lcd.clear()
        lcd.set_cursor(5, 0)
        lcd.write(ICON_UNLOCKED)
        lcd.print(" ")
        lcd.write(ICON_ARROW)
        lcd.print(" ")
        lcd.write(ICON_LOCKED)
        safe_state.lock()
        do_lock()
        show_wait_screen(100)


def safe_locked_logic():
    lcd.clear()
    lcd.set_cursor(0, 0)
    lcd.write(ICON_LOCKED)
    lcd.print(" Safe Locked! ")
    lcd.write(ICON_LOCKED)

    user_code = input_secret_code()
    unlocked  = safe_state.unlock(user_code)
    show_wait_screen(200)

    if unlocked:
        show_unlock_message()
        do_unlock()
    else:
        lcd.clear()
        lcd.set_cursor(0, 0)
        lcd.print("Access Denied!")
        show_wait_screen(1000)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def setup():
    init_icons(lcd)
    if safe_state.locked():
        do_lock()
    else:
        do_unlock()
    show_startup_message()


def main():
    setup()
    while True:
        if safe_state.locked():
            safe_locked_logic()
        else:
            safe_unlocked_logic()


main()
