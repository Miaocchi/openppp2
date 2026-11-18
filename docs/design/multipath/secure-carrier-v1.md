# Multipath Secure Carrier v1

Status: implementation draft for independent protocol review.

This protocol gives TCP, WS, and WSS paths the same client-to-origin security
layer. WSS still validates its ordinary TLS certificate; the inner Noise PSK
handshake protects traffic when a CDN terminates that outer TLS session. This
is separate from the frozen `noise-psk-v1` carrier binding. It does not change
that protocol's prologue, exporter, confirmation, or binding semantics.

## Authentication inputs and prologue

The primitive is the existing strict two-message
`Noise_NNpsk0_25519_ChaChaPoly_SHA256` handshake. The client sends message 1
(48 bytes); the server replies with message 2 (48 bytes). No application data
or 0-RTT is accepted until both sides finish Noise. A bad proof, unexpected
message size, or state transition permanently fails the channel.

Noise prologue bytes are the following concatenation. All lengths are unsigned
16-bit big-endian byte counts; strings are UTF-8 bytes, with no terminator or
normalization performed by the protocol implementation:

| Field | Encoding |
|---|---|
| Domain | ASCII `openppp2-multipath-secure-channel-v1` |
| Version | `0x0001` |
| Client identity | length, then 1..255 bytes |
| Server identity | length, then 1..255 bytes |
| Key ID | length, then 1..64 bytes |
| Capability offer | length, then 0..1024 opaque canonical bytes |

These values are included in the Noise transcript and thus authenticated by the
PSK. They are claims bound to the configured PSK, not a directory or identity
validation system: the caller must select/authorize the PSK and compare the
expected identities/key ID before accepting the channel. The role is fixed by
the endpoint implementation (network client initiator or network server
responder), not negotiated from peer input. Capability bytes must already be
canonical and must be checked by the application before enabling a feature.

## Raw protocol selection

An outer transport must select this protocol with an explicit, versioned
multipath preface before calling this class. The preface must be unambiguous,
must not be confused with legacy `ITransmission` handshake bytes, and the
server must accept it only when the multipath feature is configured on. This
class intentionally starts at the Noise message boundary and does not define
that listener gate or its fallback behavior. Until the listener gate is
implemented, this class alone is not deployable as an ingress protocol.

## Key schedule

After Noise completion, let `E` be its exporter secret and `H` the Noise
handshake hash. The implementation computes HMAC-SHA-256 keyed by `E` over
these exact byte strings:

```
openppp2-multipath-secure-channel-v1 || uint8(label_length) || label || H
```

Labels are the ASCII strings `client-to-server`, `server-to-client`, and
`exporter`; each output is 32 bytes. The first two keys are never shared
between directions. The local send/receive role selects the appropriate key.
Application join proofs use `Export(label, context, output)`, an HKDF-SHA-256
expand with the separately derived exporter key as PRK. Its info is
`openppp2-multipath-export-v1 || uint8(label_length) || label ||
uint32_be(context_length) || context`; labels are 1..128 bytes, context is at
most 4096 bytes, and output is 1..8160 bytes. Export labels are owned by the
caller and must be domain-specific.

## Ordered record layer

Each record is one complete frame:

```
uint32_be(frame_length) || ciphertext || tag
```

`frame_length` includes ciphertext plus the 16-byte Poly1305 tag and is at most
16,401 bytes. The encrypted plaintext is one record-type byte followed by 0 to
16,384 payload bytes. The four-byte length is authenticated. Additional
associated data is `length_header || uint64_be(sequence)`. The 96-bit
ChaCha20-Poly1305 nonce is four zero bytes followed by the 64-bit big-endian
sequence number. Each direction starts at sequence zero and accepts only its
next expected value; the sequence is implicit on wire. Truncation, extra frame
bytes, over-limit lengths, replay, reordering, or tag failure closes the
channel. Counters never wrap; a connection must be replaced before exhaustion.

The record type is encrypted and is not interpreted by the carrier. VMUX
retransmissions are resealed as new records on the target carrier, never
replayed as ciphertext. This layer has no stream resynchronization: callers
must read exactly the announced bounded frame before invoking `Open`.

## API and lifecycle

`MultipathSecureChannel` does not own a socket or `ITransmission`. The caller
performs exact reads/writes for handshake messages and records, serializes
concurrent access, applies deadlines, and disposes the transport on any failure.
The class is move-only, clears Noise and traffic secrets on destruction, and
becomes unusable after protocol or AEAD failure. Handshake timeouts, cancellation,
raw preface negotiation, key lookup, key rotation, and session creation/join
state are outside this primitive and must be enforced by its wrapper.

The test-only deterministic ephemeral-key hook exists solely for fixed-vector
testing. Production callers must not use it.

## Deterministic vector

The C++ test uses PSK bytes `00..1f`, client ephemeral private bytes `20..3f`,
server ephemeral private bytes `40..5f`, identities `client-a` and `server-a`,
key ID `key-1`, and capability bytes `01 02 03`. The canonical prologue is:

```
6f70656e707070322d6d756c7469706174682d7365637572652d6368616e6e656c2d7631
00010008636c69656e742d6100087365727665722d6100056b65792d310003010203
```

The corresponding Noise messages are:

```
m1 = 358072d6365880d1aeea329adf9121383851ed21a28e3b75e965d0d2cd166254
     c6acc417ab3bebce4905ccd359cade25
m2 = 79a631eede1bf9c98f12032cdeadd0e7a079398fc786b88cc846ec89af85a51a
     36ad93c1f60867b05d6d753b6b6fae09
```

For client-to-server record sequence 0, type `07`, payload `09 08 07 06`,
the complete wire record is:

```
000000157b090f7b65e3c4d103c817d62fcc26ceba132a794a
```

The 64-byte exporter output for label `join` and context `01 02` is:

```
ea243ede9e43a274d142a683b9153baf592002b31ae49b2f1eac623a9b4b381f
daa6a8a4b51f10daafd3792e477915f94ed5e37990c98d705badb327c5f5f406
```

The server-to-client sequence-zero record for the same type and payload is:

```
0000001569c06d4da36bdf7cde45c476a81851455321d4b095
```

Full public test vectors, including handshake hash and both traffic keys, are
in `tests/crypto/multipath_v1_vectors.json`. The independent reference uses
`noiseprotocol==0.3.1` for the standard Noise handshake and Python
`cryptography` for the application AEAD. It agrees byte-for-byte with the C++
tests for both handshake messages, both record directions and the contextual
exporter. These are deterministic test secrets, never deployment keys.

Reproduce in a disposable virtual environment:

```sh
python3 -m venv build/acceptance-venv
build/acceptance-venv/bin/pip install -r tests/crypto/requirements-multipath.txt
build/acceptance-venv/bin/python tests/crypto/multipath_reference_vectors.py
```

## Review and remaining deployment work

This is an implementation specification, not a claim of completed protocol
review. Before deployment, freeze a raw listener preface and feature gate,
validate identity-to-key authorization policy, standardize UTF-8 normalization
or restrict identity syntax, and review this custom key derivation and framing. Existing legacy
carrier binding remains unchanged and must not be treated as this secure
channel.
