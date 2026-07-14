"""Host mirror of temp_pin.c's accept-once/expiry/revoke logic.

Not the C code itself (no host C toolchain on this box) — a faithful port of
the same state machine, so a logic regression in temp_pin.c should be caught by
porting the change here too. Run: python test_temp_pin_logic.py
"""

class TempPin:
    def __init__(self): self.pin=""; self.exp=0; self.used=True; self.now=0
    def set(self, pin, ttl_s):
        if not pin or ttl_s <= 0:
            self.pin=""; self.exp=0; self.used=True
        else:
            self.pin=pin[:8]; self.exp=self.now+ttl_s; self.used=False
    def try_(self, entered):
        if not entered: return False
        if self.pin and not self.used:
            if self.now >= self.exp:
                self.pin=""; self.used=True; return False
            if entered == self.pin:
                self.used=True; return True
        return False

t = TempPin()
t.set("4729", 60)
assert t.try_("0000") is False          # wrong pin
assert t.try_("4729") is True           # first use ok
assert t.try_("4729") is False          # one-shot: dead after first use

t.set("1111", 10); t.now += 11          # advance past ttl
assert t.try_("1111") is False          # expired

t.set("2222", 60); t.set("", 0)         # revoke
assert t.try_("2222") is False

print("temp_pin logic self-check PASS")
