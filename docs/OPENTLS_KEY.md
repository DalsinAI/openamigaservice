# opentls.key/1

TLS public-key work for an Amiga: certificate and handshake signatures, and
the key exchange. The Amiga keeps TLS itself, end to end; this service only
does the slow maths (about 1 to 2.5 s per operation on a 68040, a few
milliseconds here). Host code: `host/opentls_key.c` (OpenSSL 3).

| Op | Name | Request | Answer |
| --- | --- | --- | --- |
| 1 | VERIFY | `arg` algorithm; `extra[0]` digest; `extra[1]` PSS salt length (0: the digest's); buf0 the key (SubjectPublicKeyInfo, DER); buf1 the digest; buf2 the signature | result 1 good, 0 bad |
| 2 | KEYGEN | `arg` curve; buf0 (out) private key; buf1 (out) public key | result public length, aux private length |
| 3 | DERIVE | `arg` curve; buf0 private key; buf1 the peer's public key; buf2 (out) the secret | result secret length |

Algorithms: 1 RSA PKCS#1 v1.5, 2 RSA-PSS (MGF1 with the same digest),
3 ECDSA (DER signature), 4 Ed25519 (buf1 is the message).
Digests: 1 SHA-1, 2 SHA-256, 3 SHA-384, 4 SHA-512.
Curves: 1 X25519 (32-byte keys), 2 P-256 (32-byte private, 65-byte
uncompressed public), 3 P-384 (48 and 97).

Status: 0, or -2 for a request it cannot use (unknown algorithm, a key that
does not parse), -4 when an output buffer is too small.

**What the host learns.** VERIFY uses public data only. KEYGEN and DERIVE
give the host the connection's key exchange, and so its session keys: use
them only with a board in this machine or a paired Cradle you trust.
