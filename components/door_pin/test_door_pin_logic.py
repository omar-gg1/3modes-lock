"""Host-side mirror of door_pin.c's state machine — no C toolchain available.
Asserts: starts at factory default, accepts a valid new PIN, rejects blank/short/
non-digit (never blanks itself out), persists the last valid set.
Run: python test_door_pin_logic.py
"""

DEFAULT = "1234"
MIN, MAX = 4, 8


class DoorPin:
    def __init__(self):
        self.pin = DEFAULT  # RAM copy, seeded from "NVS default"

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


def test():
    d = DoorPin()
    # factory default
    assert d.matches("1234")
    assert not d.matches("0000")
    assert not d.matches("")

    # valid change adopts
    assert d.set("571902")
    assert d.matches("571902")
    assert not d.matches("1234")  # old default no longer works

    # invalid changes rejected, PIN stays intact (can't lock yourself out)
    assert not d.set("")       # blank
    assert not d.set("12")     # too short
    assert not d.set("123456789")  # too long
    assert not d.set("12a4")   # non-digit
    assert d.matches("571902")  # still the last good one

    print("door_pin logic OK")


if __name__ == "__main__":
    test()
