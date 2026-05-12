#!/usr/bin/env python3
"""test_nodebus_model.py — self-tests for nodebus_model.py.

The model is the oracle for the C unit tests, so it gets its own checks
against the values PROTOCOL.md "CPU ↔ netbridge UART bridge protocol"/"Nodebus (cipher, hash, framing)" already pins down. Run via run_tests.sh
(or directly: `python3 test_nodebus_model.py`).
"""
import sys

import nodebus_model as m

_pass = 0
_fail = 0


def check(cond, what):
    global _pass, _fail
    if cond:
        _pass += 1
    else:
        _fail += 1
        print(f"  FAIL: {what}")


def hx(b: bytes) -> str:
    return " ".join(f"{x:02x}" for x in b)


# --------------------------------------------------------------------------- #
# Framing
# --------------------------------------------------------------------------- #
def test_checksum_examples():
    # PROTOCOL.md "CPU ↔ netbridge UART bridge protocol" worked examples.
    check(m.sum_zero_csum(b"\x30\x01") == 0xCF, "sum_zero_csum(30 01) == cf")
    check(m.sum_zero_csum(b"\x20\x01") == 0xDF, "sum_zero_csum(20 01) == df")
    check((sum(b"\x30\x01") + m.sum_zero_csum(b"\x30\x01")) % 256 == 0, "csum zeroes the sum")


def test_logical_frames():
    # PROTOCOL.md "CPU ↔ netbridge UART bridge protocol": cmd 0x30 request frame is 02 30 01 cf 03; cmd 0x20 is 02 20 01 df 03.
    check(m.bridge_logical_frame(0x30, 0x01) == bytes.fromhex("0230 01cf 03".replace(" ", "")),
          f"cmd 0x30 logical frame ({hx(m.bridge_logical_frame(0x30, 0x01))})")
    check(m.bridge_logical_frame(0x20, 0x01) == bytes.fromhex("022001df03"),
          f"cmd 0x20 logical frame ({hx(m.bridge_logical_frame(0x20, 0x01))})")
    # CRC32 handshake response: cmd 0x20, payload = NETBRIDGE_CRC32 little-endian.
    crc_le = m.NETBRIDGE_CRC32.to_bytes(4, "little")
    fr = m.bridge_logical_frame(0x20, 0x01, crc_le)
    check(fr[1] == 0x20 and fr[3:7] == crc_le and (sum(fr[1:-1]) % 256 == 0),
          f"CRC32 response frame well-formed ({hx(fr)})")


def test_escape_roundtrip():
    # A reserved byte in the payload must escape; STX/ETX framing stays literal.
    enc = m.escape_encode(bytes.fromhex("0202 03".replace(" ", "")))
    check(enc == bytes.fromhex("021b8203"), f"STX payload -> 1b 82 ({hx(enc)})")
    # No reserved bytes: unchanged.
    check(m.escape_encode(bytes.fromhex("023001cf03")) == bytes.fromhex("023001cf03"), "no-escape frame unchanged")
    # Round-trip over frames carrying every reserved byte.
    for logical in (bytes.fromhex("024107 0203 1baa5503".replace(" ", "")),
                    bytes.fromhex("021b021b0303")):
        wire = m.escape_encode(logical)
        check(m.escape_decode(wire) == logical, f"decode(encode(x)) == x for {hx(logical)}")
    # Malformed wire frames decode to None.
    check(m.escape_decode(b"\x30\x01\xcf\x03") is None, "no STX -> None")
    check(m.escape_decode(b"\x02\x03") is None, "too short -> None")


# --------------------------------------------------------------------------- #
# cmd-0x41 cipher builder (PROTOCOL.md "NODEBUS_Cipher — Stern's per-slot XOR stream cipher" reference)
# --------------------------------------------------------------------------- #
def test_cipher_known_vector():
    # PROTOCOL.md "NODEBUS_Cipher — Stern's per-slot XOR stream cipher": with zero keys (boot state),
    #   nodebus_encrypt_frame(slot=5, inner_cmd=0xfc, payload=b'', response_len=0x10, key_block=zeros)
    # returns 85 01 ae cc — the first-ever cmd-0xfc key-fetch frame for slot 5.
    got = m.nodebus_encrypt_frame(5, 0xFC, b"", 0x10, b"\x00" * 16)
    check(got == bytes.fromhex("8501aecc"), f"cipher slot=5 cmd=0xfc -> 85 01 ae cc (got {hx(got)})")
    # Involution: re-running the XOR loop on the encrypted cmd byte inverts it.
    enc_cmd = got[2]
    check(enc_cmd ^ 0x00 ^ ((5 << 4) | ((1 + 1) & 0x0F)) == 0xFC,
          "decrypting the cmd byte recovers 0xfc")
    # The frame's trailing checksum byte makes sum(whole frame) ≡ 0 (mod 256).
    check(sum(got) % 256 == 0, "cipher frame trailing checksum zeroes the whole-frame sum")


# --------------------------------------------------------------------------- #
# cmd-0x41 inbound validator (the 3 gates)
# --------------------------------------------------------------------------- #
def test_validator():
    # A well-formed node reply (payload the caller should see = 3 bytes, status 0)
    # round-trips: make_node_reply -> validate_node_reply -> same payload, accepted.
    payload = b"\x11\x22\x33"
    inner = m.make_node_reply(payload, status=0x00)   # 5 bytes: payload || cksum || status
    n_wire = len(inner)
    r = m.validate_node_reply(inner, n_wire)
    check(r.accepted and r.payload == payload, f"valid reply accepted, payload echoed ({r})")

    # Gate 1: wrong size.
    check(m.validate_node_reply(inner[:-1], n_wire).last_error == m.ERR_SIZE, "short reply -> ERR_SIZE")
    check(m.validate_node_reply(inner + b"\x00", n_wire).last_error == m.ERR_SIZE, "long reply -> ERR_SIZE")

    # Gate 2: status bits 2 / 3 set.
    bad2 = m.make_node_reply(payload, status=0x04)
    bad3 = m.make_node_reply(payload, status=0x08)
    check(m.validate_node_reply(bad2, len(bad2)).last_error == m.ERR_STATUS_BIT2, "status bit2 -> ERR_STATUS_BIT2")
    check(m.validate_node_reply(bad3, len(bad3)).last_error == m.ERR_STATUS_BIT3, "status bit3 -> ERR_STATUS_BIT3")

    # Gate 3: bad inner checksum (corrupt a payload byte without fixing cksum).
    corrupt = bytearray(inner)
    corrupt[0] ^= 0xFF
    check(m.validate_node_reply(bytes(corrupt), n_wire).last_error == m.ERR_CKSUM, "bad inner cksum -> ERR_CKSUM")


def main():
    test_checksum_examples()
    test_logical_frames()
    test_escape_roundtrip()
    test_cipher_known_vector()
    test_validator()
    print(f"\n[test_nodebus_model] {_pass} passed, {_fail} failed")
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
