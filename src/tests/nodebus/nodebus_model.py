"""nodebus_model.py — golden model of the netbridge UART protocol, CPU side.

Independent re-implementation of the *verified* pieces of PROTOCOL.md "CPU ↔ netbridge UART bridge protocol" + "Nodebus (cipher, hash, framing)",
used as the oracle for the nodebus unit tests. When this model and
shim_nodebus.c disagree, exactly one of them is wrong — track it down rather
than adjusting the model to match the shim.

Scope today: bridge frame framing (escape codec + sum-zero checksum), the
netbridge CRC32 constant, the cmd-0x41 encrypted-outbound builder (verbatim
from PROTOCOL.md "NODEBUS_Cipher — Stern's per-slot XOR stream cipher"), and the cmd-0x41 inbound reply validator. The bridge
dispatch matrix (request cmd -> response cmd / payload) and the cmd-0x41
inner-command response contracts are deliberately NOT modelled yet — they
land here once re-verified against the binary in the catalog phase.

NAMING CAUTION (a known PROTOCOL.md "NODEBUS_Cipher — Stern's per-slot XOR stream cipher" vs shim_nodebus.c collision): the
"hint" byte means two different things depending on which side you're on.
The wrapper passes an expected-payload-length; `mcp_nodebus_send_encrypted`
bumps it by 2 and writes that (call it `n_wire`) onto the wire. The shim's
parser reads `n_wire` and confusingly stores it in a variable also named
`hint`. Here we use `n_wire` for the on-wire byte and `payload_len` for what
the caller ultimately sees (`payload_len == n_wire - 2`).
"""
from __future__ import annotations

NETBRIDGE_CRC32 = 0x357F5327   # zlib CRC32 over netbridge.elf .text+.rodata+.binary_info

RESERVED = (0x02, 0x03, 0x1B)  # STX / ETX / ESC


# --------------------------------------------------------------------------- #
# Framing
# --------------------------------------------------------------------------- #
def sum_zero_csum(data: bytes) -> int:
    """The single byte b such that (sum(data) + b) % 256 == 0."""
    return (-sum(data)) & 0xFF


def bridge_logical_frame(cmd: int, frame_id: int, payload: bytes = b"") -> bytes:
    """Logical (un-escaped) bridge frame: STX, CMD, ID, payload..., CKSUM, ETX.
    CKSUM is chosen so (CMD + ID + payload... + CKSUM) % 256 == 0. STX/ETX are
    not part of the checksum (PROTOCOL.md "CPU ↔ netbridge UART bridge protocol")."""
    body = bytes([cmd & 0xFF, frame_id & 0xFF]) + bytes(payload)
    return bytes([0x02]) + body + bytes([sum_zero_csum(body), 0x03])


def escape_encode(logical: bytes) -> bytes:
    """Logical frame -> on-wire frame. Any CMD/ID/PAYLOAD/CKSUM byte equal to a
    reserved value goes out as 0x1B (byte | 0x80). STX/ETX framing bytes are
    emitted literally (PROTOCOL.md "CPU ↔ netbridge UART bridge protocol")."""
    if len(logical) < 2 or logical[0] != 0x02 or logical[-1] != 0x03:
        raise ValueError("logical frame must be STX ... ETX")
    out = bytearray([0x02])
    for b in logical[1:-1]:
        if b in RESERVED:
            out += bytes([0x1B, b | 0x80])
        else:
            out.append(b)
    out.append(0x03)
    return bytes(out)


def escape_decode(wire: bytes) -> bytes | None:
    """On-wire frame -> logical frame. Returns None on a malformed frame
    (missing STX/ETX, too short). Mirrors shim's bridge_decode_escape."""
    if len(wire) < 5 or wire[0] != 0x02 or wire[-1] != 0x03:
        return None
    out = bytearray([0x02])
    i = 1
    while i < len(wire) - 1:
        if wire[i] == 0x1B and i + 1 < len(wire) - 1:
            out.append(wire[i + 1] & 0x7F)
            i += 2
        else:
            out.append(wire[i])
            i += 1
    out.append(0x03)
    return bytes(out)


def bridge_frame(cmd: int, frame_id: int, payload: bytes = b"") -> bytes:
    """Convenience: logical frame, escape-encoded, ready for the wire."""
    return escape_encode(bridge_logical_frame(cmd, frame_id, payload))


# --------------------------------------------------------------------------- #
# cmd-0x41 encrypted outbound builder (PROTOCOL.md "NODEBUS_Cipher — Stern's per-slot XOR stream cipher" reference, verbatim)
# --------------------------------------------------------------------------- #
def nodebus_encrypt_frame(slot: int, inner_cmd: int, payload: bytes,
                          response_len: int, key_block: bytes) -> bytes:
    """Build and encrypt a cmd-0x41 *cipher frame* (the bytes that become the
    bridge cmd-0x41 payload, before STX/CMD/ID/CSUM/ETX wrapping).

      slot         : 5-bit slot index (0..31)
      inner_cmd    : the command byte the destination node sees
      payload      : bytes after inner_cmd (each will be encrypted)
      response_len : caller's expected response length (overwritten by checksum)
      key_block    : 16 bytes from mcp_nodebus_cipher_keys[slot*16 .. +16]

    Returns the on-the-wire cipher frame (length = inner_len + 3). The cipher
    is involutive: re-running the XOR loop inverts it.
    """
    assert 0 <= slot < 32 and len(key_block) == 16
    inner_len = len(payload) + 1
    frame = bytearray()
    frame.append(slot | 0x80)
    frame.append(inner_len & 0xFF)
    frame.append(inner_cmd & 0xFF)
    frame.extend(payload)
    frame.append(response_len & 0xFF)             # placeholder; checksum overwrites

    mask = (slot << 4) | ((inner_len + 1) & 0x0F)
    for i in range(inner_len):
        k_table = key_block[i & 0x0F]
        k_mask = (~mask) & 0xFF if (i & 1) else (mask & 0xFF)
        frame[2 + i] = frame[2 + i] ^ k_table ^ k_mask

    csum = (-sum(frame[: inner_len + 2])) & 0xFF
    frame[inner_len + 2] = csum
    return bytes(frame)


# --------------------------------------------------------------------------- #
# cmd-0x41 inbound reply validator (the 3 gates in PROTOCOL.md "NODEBUS_Cipher — Stern's per-slot XOR stream cipher")
# --------------------------------------------------------------------------- #
# mcp_nodebus_last_error codes (0 = ok).
ERR_OK, ERR_STATUS_BIT2, ERR_STATUS_BIT3, ERR_CKSUM, ERR_SIZE = 0, 1, 2, 3, 4


class NodeReply:
    def __init__(self, last_error: int, payload: bytes = b""):
        self.last_error = last_error
        self.payload = payload
        self.accepted = last_error == ERR_OK

    def __repr__(self) -> str:
        return f"NodeReply(last_error={self.last_error}, payload={self.payload.hex()})"


def validate_node_reply(inner: bytes, n_wire: int) -> NodeReply:
    """Model of mcp_nodebus_send_encrypted's reply validation.

      inner  : the inner bytes the bridge relays back, after STX/CMD/ID/CSUM/ETX
               framing is stripped (i.e. what the shim writes between the
               response cmd byte and the bridge checksum).
      n_wire : the on-wire response-length byte (== the wrapper's expected
               payload length + 2; == the shim's `hint` variable).

    Gates, in order:
      1. len(inner) must equal n_wire        -> else ERR_SIZE
      2. inner[n_wire-1] (status) bits 2,3   -> set => ERR_STATUS_BIT2 / _BIT3
      3. sum(inner[:n_wire-1]) % 256 == 0    -> else ERR_CKSUM   (the byte at
         inner[n_wire-2] is the node's cksum, included in this sum)
    On all-pass the caller sees inner[:n_wire-2]  (== payload_len bytes).
    """
    if len(inner) != n_wire:
        return NodeReply(ERR_SIZE)
    status = inner[n_wire - 1]
    if status & 0x04:
        return NodeReply(ERR_STATUS_BIT2)
    if status & 0x08:
        return NodeReply(ERR_STATUS_BIT3)
    if sum(inner[: n_wire - 1]) % 256 != 0:
        return NodeReply(ERR_CKSUM)
    return NodeReply(ERR_OK, bytes(inner[: n_wire - 2]))


def make_node_reply(payload: bytes, status: int = 0x00) -> bytes:
    """Inverse helper: given the `payload_len` bytes the caller should see and a
    status byte (bits 2,3 must be clear to be accepted), produce the `n_wire`
    inner bytes a well-formed node reply would carry. Useful for asserting that
    a synthesized reply round-trips through validate_node_reply().

    Layout: inner = payload || cksum || status, where cksum sits at
    inner[n_wire-2] and status at inner[n_wire-1]. The validator requires
    sum(inner[:n_wire-1]) = sum(payload || cksum) ≡ 0 (mod 256), so
    cksum = (-sum(payload)) & 0xFF.
    """
    body = bytes(payload)
    cksum = (-sum(body)) & 0xFF
    return body + bytes([cksum, status & 0xFF])
