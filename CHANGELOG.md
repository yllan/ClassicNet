# Changelog

All notable changes to ClassicNet are documented here. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/); the project predates formal
version tags, so this first entry captures the current, verified state.

## [Unreleased]

### Changed

- **HPACK decode buffers** (`cn_hpack.h`): `CN_HPACK_STR_MAX` 4 KB -> 16 KB.
  A single response header larger than the scratch aborted the whole HTTP/2
  response with `kCNErrHpackOverflow`; real sites (Wikipedia article pages)
  ship multi-KB `Content-Security-Policy`/`Set-Cookie` headers. The decoder
  now holds them; the collector still drops values beyond `CN_HTTP_MAX_VALUE`
  (same policy as the HTTP/1.x parser). Dynamic-table entries larger than the
  4 KB arena remain un-indexed per RFC 7541.

- **HTTP/1.x header parsing** (`cn_http.c`): ordinary header fields that cannot
  be stored — more than `CN_HTTP_MAX_HEADERS`, or a name/value exceeding
  `CN_HTTP_MAX_NAME`/`CN_HTTP_MAX_VALUE` — are now **dropped** instead of
  failing the whole response with `kCNErrTooManyHeaders`/`kCNErrHeaderTooLong`.
  Real web servers routinely send multi-KB `Content-Security-Policy` values,
  which made whole pages unreachable for CyberCat. `Content-Length` and
  `Transfer-Encoding` are preserved preferentially because `cn_request.c` needs
  them for safe body framing; an oversized framing header still fails.
  (`tests/test_http.c` and `tests/test_request_flow.c` updated; the error codes
  remain defined for source compatibility.)

## [0.1.0] — 2026-07-05

First public cut. The full stack is verified end-to-end on real Mac OS 9.2.2
(PowerPC, under QEMU). The `0.x` version reflects the known caveats below, not
immaturity of the code paths.

### Added

- **Transport** — async Open Transport (OTTCP) `CNTransport` (non-blocking,
  notifier + `CN_Idle` pump).
- **TLS** — mbedTLS-backed TLS 1.2 **and 1.3** (`cn_tls.c`); ALPN
  (`CN_TlsSetAlpn`/`CN_TlsGetAlpn`); entropy injection hook (`CN_TlsAddEntropy`).
- **Certificate verification** — fail-closed chain + hostname + validity-date
  checking when a CA bundle is supplied; `scripts/gen-ca-bundle.sh` builds an
  embedded Mozilla/curl root bundle; `scripts/gen-test-pki.sh` for the demo CA.
- **HTTP/1.1** — request state machine, streaming bodies, chunked decoding
  (`cn_request.c`, `cn_http.c`).
- **HTTP/2** — framing + SETTINGS (`cn_h2.c`), HPACK (RFC 7541) decode/encode
  (`cn_hpack.c`), connection layer with **N-way multiplexing**, flow control, and
  GOAWAY/RST_STREAM handling (`cn_h2conn.c`).
- **WebSocket** — `wss://` framing (`cn_ws.c`), SHA-1 / Base64 accept (`cn_sha1.c`,
  `cn_base64.c`), URL parsing (`cn_url.c`).
- **On-target apps** — `cnhttp`, `cnhttps`, `cnh2`, `cntest`, and a `shlb` demo;
  Mac Time Manager → mbedTLS bridge (`target/cn_mac_time.c`).
- **Tooling** — host build under ASan/UBSan, libFuzzer targets for every wire
  parser, GitHub Actions CI (host tests + fuzz smoke).

### Verified

- Real OS 9.2.2 (QEMU): HTTPS (TLS 1.3, `200`), HTTP/2 over HTTPS with ALPN `h2`
  and certificate verification (`200`), plain HTTP, and a 15/15 on-target check
  run (big-endian, zero endianness bugs).
- Host: full unit suite green under ASan+UBSan; parsers fuzzed for millions of
  iterations with no crash/OOB/UB; HPACK against RFC 7541 Appendix C vectors.

### Hardened (pre-release review rounds)

Adversarial reviews of the wire-facing code, each fix with a host regression
test (suite stays 11/11 green; http/h2conn fuzzers re-run clean):

- HTTP/1: request-line/header injection rejected (control/CRLF bytes in method,
  path, host, header names/values); framing per RFC 7230 §3.3.3 — chunked wins
  over Content-Length, conflicting/duplicate framing rejected, `HEAD` completes
  after headers, interim 1xx responses skipped.
- HTTP/2: control-frame validation per RFC 7540 §6 (SETTINGS/PING/WINDOW_UPDATE/
  GOAWAY/RST_STREAM/PRIORITY length + stream-id rules); response HEADERS without
  `:status` fail the stream; SETTINGS_INITIAL_WINDOW_SIZE overflow is a
  connection error; HEADERS queued atomically; request-header parameters
  validated; HPACK encode buffer enlarged to 4 KB.
- Misc: Base64 length-overflow guard; over-long Open Transport hostnames fail
  (`kCNErrHostTooLong`) instead of truncating; failed `CN_TlsCreate` frees all
  partially-initialized mbedTLS contexts; HPACK/frame scratch moved off the
  cooperative stack.

### Security / known limitations

See [SECURITY.md](SECURITY.md) for the full threat model and production checklist.

- **Entropy floor (highest priority).** No hardware RNG; `mbedtls_hardware_poll`
  (cy384 fork) is mediocre. `CN_TlsAddEntropy` mixes in session jitter but does not
  guarantee a strong seed. Collect ample user-input entropy in production.
- **You must supply CA roots.** No system trust store; build with
  `-DCN_CA_BUNDLE=ON` and keep the bundle current. No OCSP/CRL revocation checking.
- **TLS 1.3** is the default and works for both HTTP/1.1 and HTTP/2 (an early h2
  `-30082` was a NewSessionTicket-handling bug, now fixed — not a fork
  limitation). `CN_TLS_FORCE_TLS12` pins 1.2.
- **CFM `shlb` export limit:** a Retro68 `MakePEF` bug breaks exports >9 symbols;
  static linking is the reliable delivery form.
- **Single-threaded HPACK** (static scratch buffers; fine under cooperative
  scheduling).

### Notes

- Licensed Apache-2.0 (see `LICENSE`, `NOTICE`). mbedTLS / Retro68 and the CA
  bundle are fetched at build time, not redistributed here.
