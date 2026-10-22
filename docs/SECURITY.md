# Security notes

## Cryptography and TLS have not been audited

Zell ships its own cryptographic primitives (`runtime/src/crypto/`: AES and
AES-GCM, ChaCha20-Poly1305, X25519, Ed25519, P-256 ECDH/ECDSA, RSA
PKCS#1 v1.5/PSS, SHA-2, HMAC, HKDF) and its own TLS 1.3 / TLS 1.2
implementation and X.509 verifier written in Zell
(`compiler/src/sema/stdlib_src/{tls,tls_client,tls_server,x509}.zl`).
No external crypto library is used.

**This code has not had an external security audit.** It is tested against
RFC, NIST and Wycheproof-style vectors (`runtime/tests/test_crypto.c`), the
RFC 8448 key schedule (`tests/spec/stdlib/tls_rfc8448.zl`), a full TLS 1.3
handshake capture (`tests/spec/stdlib/tls_trace13.zl`), interoperability
with OpenSSL in both directions (`compiler/tests/tls.rs`) and a fuzz smoke
test, but tests do not prove the absence of side channels, parsing bugs or
protocol flaws. Until an audit has happened, do not rely on it where a
compromise would be costly; report problems as issues.

What the implementation tries to guarantee, and where it is known to fall
short:

- **Constant time.** Operations on secrets avoid secret-dependent branches
  and memory indices: bitsliced AES and table-free GHASH (or AES-NI and
  PCLMULQDQ), ChaCha20/Poly1305, the X25519 ladder, Ed25519 and P-256
  scalar multiplication (conditional moves, fixed windows scanned in full),
  mod-L reduction, RSA private exponentiation (fixed window, full table
  scans) with exponent blinding, constant-time MAC/tag comparison. Not
  verified with tools such as dudect or ctgrind; the compiler could in
  principle introduce branches. Signature *verification* and RSA public
  operations work on public data and are not constant time.
- **RSA signing** uses CRT, blinds the exponents with random multiples of
  `p-1`/`q-1` and verifies every signature with the public key before
  releasing it (fault attacks). Key generation is not implemented.
- **ECDSA nonces** are derived per RFC 6979 and mixed with 32 random bytes
  (hedged), so a weak RNG alone does not leak the key.
- **Randomness** comes from `getrandom(2)` (`/dev/urandom` fallback) through
  the platform layer.
- **Secrets in memory**: the C primitives wipe their stack copies, but keys
  and traffic secrets held by Zell code live in ordinary `[]byte` objects
  that are freed without being wiped, and may be copied by the runtime.
- **TLS**: no SSLv3/TLS 1.0/1.1, no RSA key exchange, no CBC or stream
  ciphers, no compression, no renegotiation, no 0-RTT; TLS 1.2 requires
  extended_master_secret; downgrade sentinels are set and checked. The
  record layer is AEAD-only. Session tickets are encrypted with
  AES-256-GCM under a per-listener random key unless
  `ServerConfig.TicketKey` is set (tickets therefore do not survive a
  restart, and there is no key rotation). Tickets live one day.
- **X.509**: chains are built by subject/key-id matching and checked for
  signatures, validity, CA flags, path length, key usage, extended key
  usage, unknown critical extensions and dNSName name constraints. There
  is no revocation checking (CRL/OCSP), no policy processing, and only
  dNSName constraints (IP/email/URI constraints are ignored). SHA-1
  signatures are rejected; only P-256 among the NIST curves is supported,
  so chains through P-384 CAs fail to verify. `InsecureSkipVerify` turns
  off chain and host-name checks (the handshake signature is still
  checked) and must not be used outside tests.
- **System roots** are read from the PEM bundle of the distribution (or
  `$SSL_CERT_FILE`); macOS and Windows trust stores are not used yet.
