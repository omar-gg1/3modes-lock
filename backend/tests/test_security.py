from app.security import signing_string, sign_command, new_nonce


def test_signing_string_is_canonical():
    s = signing_string("lock-01", "unlock", "abcd", 100, 108, {})
    assert s == "lock-01|unlock|abcd|100|108|{}"


def test_signing_string_sorts_and_compacts_args():
    s = signing_string("lock-01", "set_temp_pin", "abcd", 100, 108,
                       {"pin": "0000", "ttl_s": 60})
    # keys sorted, no spaces
    assert s.endswith('|{"pin":"0000","ttl_s":60}')


def test_sign_command_is_64_hex_and_deterministic():
    a = sign_command("00" * 32, "lock-01", "unlock", "abcd", 100, 108, {})
    b = sign_command("00" * 32, "lock-01", "unlock", "abcd", 100, 108, {})
    assert a == b
    assert len(a) == 64
    int(a, 16)  # is hex


def test_sign_changes_with_secret():
    a = sign_command("00" * 32, "lock-01", "unlock", "abcd", 100, 108, {})
    b = sign_command("11" * 32, "lock-01", "unlock", "abcd", 100, 108, {})
    assert a != b


def test_new_nonce_is_16_hex_and_unique():
    n1, n2 = new_nonce(), new_nonce()
    assert len(n1) == 16 and len(n2) == 16
    int(n1, 16)
    assert n1 != n2
