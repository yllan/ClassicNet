# ClassicNet

A modern network stack for Classic Mac OS (PowerPC, Mac OS 8/9) — **TLS 1.2/1.3,
HTTPS, HTTP/2 (multiplexing + flow control), and WebSocket**. Built on a portable
C core (host-tested under sanitizers/fuzzers) layered over Open Transport, with
TLS provided by mbedTLS. Design details: [DESIGN.md](DESIGN.md). Security posture
and the production checklist: [SECURITY.md](SECURITY.md).

## Status

End-to-end verified on **real Mac OS 9.2.2 (PowerPC, under QEMU)**:

- **TLS 1.3 + HTTP/2 over HTTPS** — `cnh2` negotiates ALPN `h2`, with certificate
  verification on, and fetches `200`.
- **HTTPS (HTTP/1.1)** — `cnhttps` completes a real TLS handshake (chain +
  hostname + validity-date verification, fail-closed) and fetches `200`.
- **Plain HTTP** — `cnhttp` over the async Open Transport state machine.
- **Portable core** — the on-target test runner passes 15/15 checks on big-endian
  PowerPC (URL/HTTP parsing, chunked, WebSocket framing, SHA-1, Base64).

On the host (Linux/macOS) the protocol core builds under AddressSanitizer + UBSan
with 11/11 unit tests passing, and every wire parser is a libFuzzer target run for
millions of iterations with no crash/OOB/UB.

## Project structure

```
include/classicnet/   public headers (portable type seam, error codes, per-protocol API)
src/                  portable protocol code (shared by host and the Mac target)
target/               OS 9 / PowerPC apps + glue (cnhttp, cnhttps, cnh2, cntest, shlb)
tests/                host-only unit tests (run under ASan/UBSan)
fuzz/                 libFuzzer targets for the wire parsers
scripts/              build/test/QEMU helpers
```

## Build

Full instructions in [docs/BUILDING.md](docs/BUILDING.md). Quickstart for the
host tests (no special setup):

```bash
cmake -S . -B build          # AddressSanitizer + UBSan on by default
cmake --build build
ctest --test-dir build --output-on-failure
./scripts/run-fuzz.sh http 30   # url | http | ws | chunked | base64 | hpack | h2
```

The on-target build needs the Retro68 toolchain and a PowerPC mbedTLS; see
BUILDING.md §2–§6. QEMU is **not** cycle-accurate — it verifies functional
correctness, but its speed does **not** represent real G3/G4 performance.

## Limitations & caveats

Read these before trusting ClassicNet with anything real — and see
[SECURITY.md](SECURITY.md) for the full threat model and production checklist.

### Certificates / CA trust

- **Classic Mac OS ships no trust store, so you must supply the roots.** ClassicNet
  verifies against a CA bundle *you* embed. `scripts/gen-ca-bundle.sh` turns the
  Mozilla/curl root set (the system `ca-certificates.crt`, or a downloaded
  `cacert.pem` — ~147 roots, ~221 KB) into `target/ca_bundle.h` (an embedded C
  string, gitignored); build the target app with `-DCN_CA_BUNDLE=ON` to verify
  real public servers. The throwaway CA from `scripts/gen-test-pki.sh` is for the
  demo/tests only — never ship it.
- **Verification is fail-closed when a CA is supplied**: chain + validity dates +
  hostname are all checked (`MBEDTLS_SSL_VERIFY_REQUIRED`). The only unverified
  path is the explicit developer mode `caPem == 0`, which announces
  `verify: NONE (insecure)` and must never reach a release build.
- **No revocation checking** (no OCSP/CRL): a revoked-but-unexpired certificate is
  accepted.
- **RAM cost**: the full bundle parses to a few hundred KB of live X.509. On
  memory-tight machines, trim `cacert.pem` to the roots you actually need and
  regenerate. Roots expire and get distrusted — re-run `gen-ca-bundle.sh` on a
  schedule and ship updates.

### Entropy (highest-priority caveat)

- Classic Mac OS has **no hardware RNG and no `/dev/random`.** The base seed comes
  from `mbedtls_hardware_poll` in cy384's PowerPC fork, which is self-described as
  **mediocre**. A freshly booted machine with no user input cannot reach a
  cryptographically strong seed by software alone — treat such handshakes with
  suspicion.
- `CN_TlsAddEntropy()` lets the app mix in session jitter (mouse/keyboard
  inter-event timing, `Microseconds`/`TickCount` deltas) before the handshake; it
  is reseeded as DRBG *additional input* (only adds entropy, never removes).
  `cn_collect_jitter()` (`target/cn_mac_time.c`, called from `h2_get.c`/`https_get.c`)
  shows the pattern. This **improves** the
  seed but is **not a guarantee** — the weak base source remains the limiting
  factor. Production code should collect plenty of user-input entropy.

### TLS version

- **TLS 1.3 is the default** on mbedTLS 3.x (the PowerPC target); 1.2 also works.
  Define `CN_TLS_FORCE_TLS12` to pin 1.2 as a fallback. The host build links
  mbedTLS 2.28 LTS, which is 1.2-only regardless.
- **HTTP/2 runs over TLS 1.3.** An earlier `-30082` mid-stream failure was traced
  to mishandling the TLS 1.3 post-handshake NewSessionTicket; `cn_tls.c` now treats
  it as would-block, and h2-over-1.3 is verified end-to-end on real OS 9 (cnh2:
  TLSv1.3 + ALPN `h2` + `200`). The test server allows 1.3 by default
  (`CN_H2_FORCE_TLS12=1` to pin 1.2 for the fallback path).

### Other

- **Single-threaded HPACK**: the decoder uses static scratch buffers — don't decode
  on multiple threads concurrently. Classic Mac OS apps are cooperatively
  scheduled, so this holds in practice.
- **CFM shared library**: the static-link form (`cnhttp`/`cnhttps`/`cntest`) is the
  reliable delivery vehicle. A CFM `shlb` works for a small exported API (≤9
  symbols) but a Retro68 `MakePEF` export-hash bug breaks larger exports — see
  DESIGN.md §11.

## Attribution & third-party

ClassicNet is **Apache-2.0** (see [LICENSE](LICENSE) and [NOTICE](NOTICE)). The
NOTICE file is the authoritative attribution list; in short:

- **Mbed TLS** (Apache-2.0) — TLS engine. The PowerPC target links a classic-Mac
  fork by **cy384** (TLS-client config + `mbedtls_hardware_poll` entropy). Fetched
  at build time, not redistributed here; its own license applies.
- **Retro68** — cross-compiler toolchain (build dependency only).
- **HPACK static table** (`src/cn_hpack_tables.h`) is derived from **RFC 7541,
  Appendix A**; the Huffman/static tables are generated from the authoritative
  `hpack` Python package, not hand-copied.
- The embedded **CA bundle** (when you build it) is derived from the **Mozilla/curl**
  root set — keep its own terms in mind when you redistribute an app that ships it.

If you redistribute ClassicNet or an app built on it, retain the LICENSE/NOTICE and
the upstream notices for the fetched dependencies.

## License

Apache-2.0 — see [LICENSE](LICENSE) and [NOTICE](NOTICE). © 2026 Yung-Luen Lan.
mbedTLS / Retro68 and other dependencies are fetched at build time and are not
redistributed in this repository (see NOTICE).
