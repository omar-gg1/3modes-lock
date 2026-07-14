"""Host-side check of the mode_ctrl range rule (mirrors the C valid_mode()).
Keeps the 1..3 contract honest without flashing hardware."""


def valid_mode(m: int) -> bool:
    return 1 <= m <= 3


def test_accepts_1_2_3():
    assert all(valid_mode(m) for m in (1, 2, 3))


def test_rejects_out_of_range():
    for m in (0, 4, -1, 255):
        assert not valid_mode(m)
