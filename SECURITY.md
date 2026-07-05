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
- **TLS 1.3 default, 1.2 fallback.** TLS 1.3 is the default on mbedTLS 3.x and is
  verified end-to-end (host + real OS 9), for both HTTP/1.1 and HTTP/2; 1.2 also
  works. `CN_TLS_FORCE_TLS12` pins 1.2 (it is *not* pinned by default). The TLS 1.3
  post-handshake NewSessionTicket is handled as would-block in `tls_recv`/
  `tls_send` (mishandling it was the earlier on-target `-30082`).
- **Length-bounded, NUL-safe parsers.** The URL/HTTP/chunked/WebSocket/HPACK/
  HTTP-2 parsers never index past the supplied length and ignore C-string
  termination. Each is a libFuzzer target run under ASan+UBSan for millions of
  iterations with zero crashes/OOB/UB (`scripts/run-fuzz.sh`).
- **Adversarial code review, multiple rounds.** The wire-facing code (`cn_tls.c`,
  `cn_hpack.c`, `cn_h2conn.c`, `cn_request.c`, `cn_http.c`, `cn_url.c`,
  `cn_base64.c`, `cn_ot.c`) went through repeated reviews for fail-open TLS,
  memory safety on attacker input, and protocol arithmetic. Findings were fixed
  and regression-tested, notably:
  - HTTP/1 request-line/header **injection rejected** — control chars/CRLF are
    refused in method, path, host, header names, and header values
    (`cn_http.c`, `cn_url.c`).
  - HTTP/1 **framing per RFC 7230 §3.3.3** — `Transfer-Encoding: chunked` wins
    over `Content-Length` regardless of header order, conflicting or duplicate
    framing is rejected, `HEAD` completes after headers, interim 1xx responses
    are skipped (`cn_request.c`).
  - HTTP/2 **control-frame validation per RFC 7540 §6** — SETTINGS / PING /
    WINDOW_UPDATE / GOAWAY / RST_STREAM / PRIORITY length and stream-id rules,
    a response without `:status` fails the stream, and a
    SETTINGS_INITIAL_WINDOW_SIZE delta that would overflow a stream window is a
    connection error; HEADERS are queued atomically (no dangling half-frame on
    output-queue overflow) (`cn_h2conn.c`).
  - **Misc bounds** — Base64 encode rejects lengths whose output size would
    overflow; an Open Transport hostname too long for the `host:port` buffer
    fails with `kCNErrHostTooLong` instead of silently truncating (and possibly
    connecting to the wrong server); a failed `CN_TlsCreate` frees every
    partially-initialized mbedTLS context.
  - Integer-overflow guards (HPACK varint, chunk size, Content-Length) were
    checked arithmetically, not just left to the fuzz corpus.

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
   feed it via `CN_TlsAddEntropy` before the handshake (see `cn_collect_jitter` in
   `target/cn_mac_time.c`, used by `h2_get.c`/`https_get.c`). This improves the seed but is **not** a guarantee — the
   weak base entropy source remains the limiting factor.
5. **Keep the CA bundle current.** Roots expire and get distrusted; re-run
   `gen-ca-bundle.sh` on a schedule and ship updates.

## Known limitations

- **Base entropy quality (high priority).** `mbedtls_hardware_poll` (cy384's
  fork) is self-described as mediocre. `CN_TlsAddEntropy` mitigates but does not
  fix this; a machine with no real entropy source cannot reach a strong seed by
  software alone. Treat freshly-booted, no-user-input handshakes with suspicion.
- **TLS 1.3 supported (default).** Both 1.2 and 1.3 work (the post-handshake
  NewSessionTicket return is handled as would-block in `tls_recv`/`tls_send`); 1.3
  is the default on mbedTLS 3.x, verified on the host against vanilla 3.6.0 and
  real public servers, and **confirmed end-to-end on real OS 9** for both HTTP/1.1
  and HTTP/2 (cnh2 negotiated TLSv1.3 + ALPN `h2` and fetched 200). Define
  `CN_TLS_FORCE_TLS12` to pin 1.2 as a fallback. (Historical note: an early h2
  `-30082` was misattributed to the fork's 1.3 data phase; the real cause was the
  NewSessionTicket handling, now fixed — see DESIGN.md §11.)
- **No revocation checking.** No OCSP/CRL; a revoked-but-unexpired cert is
  accepted. Acceptable for many uses, but know it.
- **Single-threaded assumption.** The HPACK decoder uses static scratch buffers;
  do not decode on multiple threads concurrently (Classic Mac OS apps are
  cooperatively scheduled, so this holds in practice).

## Reporting

This is a hobby/retro project; there is no formal disclosure process. File an
issue with a reproducer.
