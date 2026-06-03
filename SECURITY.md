# ClassicNet — Security Posture & Production Checklist

ClassicNet is a TLS / HTTPS / WebSocket / HTTP-2 stack for Classic Mac OS 8/9
(PowerPC). This document states what is verified, what is hardened, and what you
**must** do before trusting it in production. It complements `DESIGN.md §11`.

## Threat model

The network and the peer are untrusted. Every byte off the wire is attacker-
controlled: TLS records, HTTP/1.x and chunked bodies, WebSocket frames, HTTP/2
frames, and HPACK header blocks. The stack must (a) **fail closed** on any
verification failure and (b) never read or write out of bounds on hostile input.
Out of scope: a malicious *local* app, physical access, and side channels.

## Verified properties

- **Fail-closed certificate verification.** When a CA is supplied, `cn_tls.c`
  sets `MBEDTLS_SSL_VERIFY_REQUIRED` and verifies the **chain + validity dates +
  hostname**. Proven on the host (`scripts/test-tls-verify.sh`: unknown CA →
  reject, hostname mismatch → reject; `scripts/test-tls-real.sh`: a real public
  server verifies against the 147-root Mozilla bundle, wrong hostname rejected)
  and on real OS 9 (expired cert rejected with `-0x2700`). The only unverified
  path is the explicit `caPem == 0` developer mode, which announces
  `verify: NONE (insecure)`.
- **No hostname fail-open.** `mbedtls_ssl_set_hostname` failure is fatal — a
  silent skip would verify the chain but not the name. (Audit-driven fix.)
- **TLS 1.2 pinned on mbedTLS 3.x.** Avoids the PowerPC fork's unreliable TLS
  1.3 data phase; the client enforces it (`mbedtls_ssl_conf_max_tls_version`),
  not the server.
- **Length-bounded, NUL-safe parsers.** The URL/HTTP/chunked/WebSocket/HPACK/
  HTTP-2 parsers never index past the supplied length and ignore C-string
  termination. Each is a libFuzzer target run under ASan+UBSan for millions of
  iterations with zero crashes/OOB/UB (`scripts/run-fuzz.sh`).
- **Independent code audit.** `cn_tls.c`, `cn_hpack.c`, `cn_h2conn.c`,
  `cn_request.c`, `cn_http.c` were reviewed for fail-open TLS, memory safety on
  attacker input, and HTTP/2 padding/length arithmetic — no findings. Integer-
  overflow guards (HPACK varint, chunk size, Content-Length) were checked
  arithmetically, not just left to the fuzz corpus.

## Production checklist

Before shipping, you **must**:

1. **Embed a real CA bundle.** Build with `-DCN_CA_BUNDLE=ON` after running
   `scripts/gen-ca-bundle.sh` (the throwaway test CA from `gen-test-pki.sh` is
   for the demo only). Trim `cacert.pem` to the roots you need if RAM is tight —
   the full bundle parses to a few hundred KB of live X.509.
2. **Verify against the real hostname.** Pass the actual server hostname to
   `CN_TlsCreate` (drives SNI + hostname check). Do not leave it `"localhost"`.
3. **Never ship the insecure path.** Ensure `caPem != 0`; treat
   `verify: NONE` as a build error in release.
4. **Harden the RNG seed.** Classic Mac OS has no hardware RNG and the built-in
   `mbedtls_hardware_poll` seed is mediocre. Collect inter-event timing jitter
   (mouse, keyboard, `Microseconds`/`TickCount` deltas) over the session and
   feed it via `CN_TlsAddEntropy` before the handshake (see `gather_jitter` in
   `target/h2_get.c`). This improves the seed but is **not** a guarantee — the
   weak base entropy source remains the limiting factor.
5. **Keep the CA bundle current.** Roots expire and get distrusted; re-run
   `gen-ca-bundle.sh` on a schedule and ship updates.

## Known limitations

- **Base entropy quality (high priority).** `mbedtls_hardware_poll` (cy384's
  fork) is self-described as mediocre. `CN_TlsAddEntropy` mitigates but does not
  fix this; a machine with no real entropy source cannot reach a strong seed by
  software alone. Treat freshly-booted, no-user-input handshakes with suspicion.
- **TLS 1.3 supported.** Both 1.2 and 1.3 work (the post-handshake
  NewSessionTicket return is handled in `tls_recv`/`tls_send`); 1.3 is the
  default on mbedTLS 3.x, verified on the host against vanilla 3.6.0 and real
  public servers. Define `CN_TLS_FORCE_TLS12` to pin 1.2 as a fallback. (The
  on-target 3.6 build uses the same code path; end-to-end target confirmation of
  1.3 is pending an emulator run.)
- **No revocation checking.** No OCSP/CRL; a revoked-but-unexpired cert is
  accepted. Acceptable for many uses, but know it.
- **Single-threaded assumption.** The HPACK decoder uses static scratch buffers;
  do not decode on multiple threads concurrently (Classic Mac OS apps are
  cooperatively scheduled, so this holds in practice).

## Reporting

This is a hobby/retro project; there is no formal disclosure process. File an
issue with a reproducer.
