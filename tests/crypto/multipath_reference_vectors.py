#!/usr/bin/env python3
"""Cross-check the C++ secure-carrier vectors with the independent Noise library.

Run in a disposable venv with requirements-multipath.txt. All key material in
this file is public deterministic test data, never production credentials.
"""

import argparse
import hashlib
import hmac
import json
import pathlib
import struct

from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from noise.connection import Keypair, NoiseConnection


DOMAIN = b"openppp2-multipath-secure-channel-v1"
EXPORT_DOMAIN = b"openppp2-multipath-export-v1"
VECTOR_FILE = pathlib.Path(__file__).with_name("multipath_v1_vectors.json")


def mac(key, data):
    return hmac.new(key, data, hashlib.sha256).digest()


def expand(prk, info, size):
    if not 0 < size <= 255 * 32:
        raise ValueError("invalid HKDF size")
    previous = b""
    output = b""
    for counter in range(1, (size + 31) // 32 + 1):
        previous = mac(prk, previous + info + bytes([counter]))
        output += previous
    return output[:size]


def field(value):
    return struct.pack("!H", len(value)) + value


def connection(initiator, private, prologue):
    peer = NoiseConnection.from_name(b"Noise_NNpsk0_25519_ChaChaPoly_SHA256")
    peer.set_as_initiator() if initiator else peer.set_as_responder()
    peer.set_psks(psk=bytes(range(32)))
    peer.set_prologue(prologue)
    peer.set_keypair_from_private_bytes(Keypair.EPHEMERAL, private)
    peer.start_handshake()
    return peer


def generate():
    prologue = DOMAIN + b"\0\1" + b"".join(
        field(value) for value in (b"client-a", b"server-a", b"key-1", b"\1\2\3")
    )
    client = connection(True, bytes(range(32, 64)), prologue)
    server = connection(False, bytes(range(64, 96)), prologue)
    # Keep the independent library state alive through its normal split so the
    # explicitly specified OpenPPP2 exporter extension can be checked as well.
    client_state = client.noise_protocol.handshake_state.symmetric_state
    server_state = server.noise_protocol.handshake_state.symmetric_state
    m1 = bytes(client.write_message())
    assert bytes(server.read_message(m1)) == b""
    m2 = bytes(server.write_message())
    assert bytes(client.read_message(m2)) == b""
    assert client.handshake_finished and server.handshake_finished
    transcript = client.get_handshake_hash()
    assert transcript == server.get_handshake_hash()
    assert client_state.ck == server_state.ck

    split = expand(mac(client_state.ck, b""), b"", 96)
    noise_exporter = split[64:96]

    def derive(label):
        return mac(noise_exporter, DOMAIN + bytes([len(label)]) + label + transcript)

    c2s = derive(b"client-to-server")
    s2c = derive(b"server-to-client")
    exporter = derive(b"exporter")
    header = struct.pack("!I", 1 + 4 + 16)
    sequence = struct.pack("!Q", 0)
    plaintext = bytes([7, 9, 8, 7, 6])
    record = header + ChaCha20Poly1305(c2s).encrypt(
        b"\0" * 4 + sequence, plaintext, header + sequence
    )
    reverse_record = header + ChaCha20Poly1305(s2c).encrypt(
        b"\0" * 4 + sequence, plaintext, header + sequence
    )
    label, context = b"join", b"\1\2"
    info = EXPORT_DOMAIN + bytes([len(label)]) + label + struct.pack("!I", len(context)) + context
    return {key: value.hex() for key, value in {
        "prologue": prologue,
        "message1": m1,
        "message2": m2,
        "handshake_hash": transcript,
        "noise_exporter": noise_exporter,
        "client_to_server_key": c2s,
        "server_to_client_key": s2c,
        "exporter_key": exporter,
        "client_record_sequence0_type7_payload09080706": record,
        "server_record_sequence0_type7_payload09080706": reverse_record,
        "export_join_context0102_64": expand(exporter, info, 64),
    }.items()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--print", action="store_true", dest="print_vectors")
    args = parser.parse_args()
    generated = generate()
    if args.print_vectors:
        print(json.dumps(generated, indent=2, sort_keys=True))
        return
    expected = json.loads(VECTOR_FILE.read_text(encoding="ascii"))
    if generated != expected:
        mismatches = sorted(key for key in generated.keys() | expected.keys()
                            if generated.get(key) != expected.get(key))
        raise SystemExit("Independent Noise vectors mismatch: " + ", ".join(mismatches))
    print("PASS: independent Noise handshake, directional keys, record and contextual exporter")


if __name__ == "__main__":
    main()
