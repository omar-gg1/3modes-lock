"""Host-side mirror of confirm_pin.c's state machine — no C toolchain available.
Asserts: starts at factory default (enabled), accepts a valid new code, rejects
blank/short/non-digit (never blanks itself out), persists the last valid set, and
the enable toggle flips independently of the code.
Run: python test_confirm_pin_logic.py
"""

DEFAULT = "0000"
ON_DEFAULT = True
MIN, MAX = 4, 8


class ConfirmPin:
    def __init__(self):
        self.pin = DEFAULT   # RAM copy, seeded from "NVS default"
        self.on = ON_DEFAULT

    @staticmethod
    def _valid(p):
        return isinstance(p, str) and MIN <= len(p) <= MAX and p.isdigit()

    def matches(self, entered):
        return bool(entered) and entered == self.pin

    def set(self, new):
        if not self._valid(new):
            return False  # unchanged
        self.pin = new
        return True

    def set_enabled(self, on):
        self.on = bool(on)


def test():
    c = ConfirmPin()
    # factory default, enabled
    assert c.matches("0000")
    assert c.on is True
    assert not c.matches("1234")
    assert not c.matches("")

    # valid change adopts
    assert c.set("824613")
    assert c.matches("824613")
    assert not c.matches("0000")  # old default no longer works

    # invalid changes rejected, code stays intact (can't lock yourself out)
    assert not c.set("")            # blank
    assert not c.set("12")          # too short
    assert not c.set("123456789")   # too long
    assert not c.set("12a4")        # non-digit
    assert c.matches("824613")      # still the last good one

    # toggle flips independently, code survives
    c.set_enabled(False)
    assert c.on is False
    assert c.matches("824613")      # code untouched by the toggle
    c.set_enabled(True)
    assert c.on is True

    print("confirm_pin logic OK")


if __name__ == "__main__":
    test()
