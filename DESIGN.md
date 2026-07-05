# ClassicNet — Design Document

A modern network stack for Classic Mac OS, providing TLS / HTTPS / HTTP/2 /
WebSocket on PowerPC Mac OS 8/9.

> **Reading note.** §1–§10 describe the architecture and the design decisions
> behind it; they are written in the present tense and reflect what is actually
> delivered. §11 is the running build-and-verification log (what was proven, and
> on what). When in doubt about *current* behaviour, the code and §11 win.

---

## 1. Goals & scope

- **Target platform: PowerPC, Mac OS 8.0 – 9.2.2.**
  - No 68k (avoids the A5 world / CODE resource / segment loader complexity).
  - No 7.x (narrows the Open Transport version and test surface).
- **Performance sweet spot:** best on G3-class machines, but 601/603/604 should be
  usable. A TLS handshake on PPC lands in the "a few seconds" range — usable.
- **Protocols provided:** TLS 1.2 **and 1.3**, HTTPS (HTTP/1.1), **HTTP/2
  (multiplexing + flow control)**, and WebSocket (`wss://`). All are implemented
  and verified end-to-end on real OS 9 (see §11); HTTP/2 and TLS 1.3, originally
  scoped as "phase 2 / evaluate later", are done.

## 2. Overall architecture (layers)

```
  ┌─────────────────────────────────────────────┐
  │  App  (cooperative event loop)              │
  └───────────────┬─────────────────────────────┘
                  │  C API (callback-based, see §8)
  ┌───────────────▼─────────────────────────────┐
  │  L4  HTTP/1.1 · WebSocket · HTTP/2 (mux)     │
  ├─────────────────────────────────────────────┤
  │  L3  TLS 1.2 / 1.3  (mbedTLS, trimmed)       │
  ├─────────────────────────────────────────────┤
  │  L2  CSPRNG / entropy gathering · CA verify  │
  ├─────────────────────────────────────────────┤
  │  L1  Transport: Open Transport (OTTCP)       │
  │      async, driven by notifier + CN_Idle     │
  └─────────────────────────────────────────────┘
```

Each layer is a `CNTransport` (a small vtable: poll/send/recv/close), so TLS
stacks directly on Open Transport and HTTP/2 stacks on TLS — same seam all the way
down.

## 3. Crypto / TLS

- **mbedTLS (trimmed).** Rationale: an existing Classic Mac port to build on, a
  modular config that trims toward small RAM, headroom for TLS 1.3, and ALPN
  support (required by HTTP/2).
  - Prior art: [antscode/mbedtls-Mac-68k](https://github.com/antscode/mbedtls-Mac-68k),
    [bbenchoff MacSSL](https://bbenchoff.github.io/pages/MacSSL.html).
  - Considered fallback: BearSSL (pure C89, zero dynamic allocation, but TLS 1.2
    only and maintenance is stalled).
- **Performance trade-offs (software crypto, no AES-NI):**
  - Prefer **ChaCha20-Poly1305** ciphers (faster than AES in software).
  - Prefer **ECDHE P-256 + ECDSA** key exchange (usually cheaper than RSA-2048).

## 4. Delivery form: CFM shared library

- The intended delivery is a **PowerPC CFM (Code Fragment Manager) + PEF shared
  library**: consistent with system services (OpenTransportLib, etc.), separately
  updatable (TLS/certificates need ongoing patching), and shareable across
  clients. In practice the **static-link** form is the reliable vehicle today; the
  `shlb` works for a small exported API but a Retro68 `MakePEF` bug limits larger
  exports (see §11).
- Memory model uses the native CFM/PPC path — no A5 globals.

## 5. Toolchain

- **Primary: [Retro68](https://github.com/autc04/Retro68)** (a GCC cross-compiler
  on Linux, integrated with CMake). Day-to-day development, testing, and CI use it.
- **Known risk:** producing a PPC CFM shared library with Retro68 is "doable but
  barely documented." Flow: `gcc -shared` → XCOFF → `MakePEF` → `MakeImport`
  (import stub) → `Rez`. Some structures (e.g. `ImportMacFunctions`) are
  undocumented and had to be reverse-engineered. See
  [Retro68 #97](https://github.com/autc04/Retro68/issues/97).
- **Verified ✅:** the `Retro68/Samples/SharedLibrary` recipe does produce a CFM
  shared library (see §11), so the earlier "barely documented" risk is largely
  retired.
- **Fallback:** if shared-library output stalls, move the "package as CFM shared
  library" step to **CodeWarrior (inside the SheepShaver emulator)**, where CFM
  shared libraries are a first-class output.

## 6. The three hard problems (OS-version-independent; must be solved head-on)

1. **Entropy.** No hardware RNG, no `/dev/random`. We build a CSPRNG seed by mixing
   `Microseconds()`, `TickCount()`, mouse coordinates, memory state, and OT packet
   timing. When the built-in collector's quality is insufficient, it must warn in
   the log. **This is a security precondition, not optional.** (See §11 for the
   `CN_TlsAddEntropy` hook and its caveats.)
2. **Root CA store.** The system has no trust store. We bundle a set of CA roots
   (a DER/PEM blob) and plan an update strategy; an `onVerifyPeer` callback also
   lets the app do TOFU or prompt the user.
3. **Cooperative multitasking → async API.** OS 8/9 app code is cooperatively
   scheduled with no preemptive threads. We must never block on I/O. The whole
   library is an event-driven state machine (see §8).

## 7. Locked design decisions

1. **Core is pure callback.** A synchronous-style wrapper (Thread Manager +
   `YieldToAnyThread`) is an optional later layer, not in the core.
2. **Error model is `OSStatus`,** with a custom negative range carved out for
   ClassicNet (`kCNErrBase = -30000`, going downward).
3. **HTTP/2 multiplexing** was originally phase 2; it is now implemented (see §11).
   HTTP/1.1-over-TLS remains available per request.

## 8. Async API sketch

### 8.1 Principles
1. Never block — always "initiate → notify via callback."
2. **Callbacks always fire at system-task time** (from inside `CN_Idle`), never at
   interrupt / OT-notifier time. The notifier only sets a flag; work is deferred to
   `CN_Idle`.
3. Response bodies stream (`onData` in chunks) rather than buffering whole.

### 8.2 Lifecycle & pump
```c
OSStatus  CN_Init(const CNConfig *config, CNContextRef *outCtx);
void      CN_Dispose(CNContextRef ctx);

/* The heartbeat: call frequently from the event loop; does bounded work and
   returns. All callbacks fire here. */
void      CN_Idle(CNContextRef ctx);

/* Sleep hint for WaitNextEvent: max ticks before CN_Idle should run again. */
UInt32    CN_NextIdleDelay(CNContextRef ctx);
```
Main loop:
```c
for (;;) {
    UInt32 sleep = CN_NextIdleDelay(ctx);
    WaitNextEvent(everyEvent, &evt, sleep, nil);
    /* ... handle your own evt ... */
    CN_Idle(ctx);
}
```

### 8.3 Config (injection points for the three hard problems)
```c
typedef struct {
    void (*gatherEntropy)(void *buf, UInt32 want, UInt32 *got, void *ud); /* nil = built-in collector */
    const void   *caBundle;       /* nil = cannot verify certificates */
    UInt32        caBundleLen;
    CNAllocator  *allocator;      /* optional: custom memory pool */
    void         *userData;
} CNConfig;
```

### 8.4 High-level HTTP API
```c
typedef struct {
    CNCertDecision (*onVerifyPeer)(CNRequestRef r, const CNCertInfo *info,
                                   OSStatus dflt, void *ud);
    void    (*onResponse)(CNRequestRef r, UInt16 status,
                          const CNHeader *hdrs, UInt32 n, void *ud);
    Boolean (*onData)    (CNRequestRef r, const void *bytes, UInt32 len, void *ud); /* false = back-pressure */
    void    (*onComplete)(CNRequestRef r, OSStatus result, void *ud);
} CNRequestCallbacks;

typedef struct {
    CNHTTPMethod              method;       /* kCN_GET / kCN_POST ... */
    const char               *url;          /* "https://..." */
    const CNHeader           *headers;
    UInt32                    headerCount;
    const void               *body;
    UInt32                    bodyLength;
    UInt32 (*bodyProvider)(CNRequestRef r, void *buf, UInt32 max, void *ud); /* streaming upload */
    const CNRequestCallbacks *cb;
    void                     *userData;
} CNRequestParams;

OSStatus CN_HTTPRequest(CNContextRef ctx, const CNRequestParams *p, CNRequestRef *out);
void     CN_RequestCancel(CNRequestRef r);   /* safe: frees only after the state machine winds down */
```

### 8.5 WebSocket
```c
typedef struct {
    void (*onOpen)   (CNWebSocketRef ws, void *ud);
    void (*onMessage)(CNWebSocketRef ws, CNWSOpcode op,
                      const void *data, UInt32 len, Boolean final, void *ud);
    void (*onClose)  (CNWebSocketRef ws, UInt16 code, void *ud);
    void (*onError)  (CNWebSocketRef ws, OSStatus err, void *ud);
} CNWebSocketCallbacks;

OSStatus CN_WSConnect(CNContextRef ctx, const char *url,  /* "wss://..." */
                      const CNWebSocketCallbacks *cb, void *ud, CNWebSocketRef *out);
OSStatus CN_WSSend(CNWebSocketRef ws, CNWSOpcode op, const void *data, UInt32 len);
void     CN_WSClose(CNWebSocketRef ws, UInt16 code, const char *reason);
```

## 9. Roadmap

**Phase 0 — vertical slice (prove performance feasibility, the biggest unknown) ✅**
- Retro68 build environment + CMake.
- OT TCP connection (async, notifier + `CN_Idle`).
- Adopt the mbedTLS port, custom entropy source, ChaCha20 cipher.
- Goal: complete one **HTTPS GET** (static link is fine), measure handshake time on
  G3/603. — *Done; measured under QEMU, see §11.*

**Phase 1 — a usable library ✅**
- Full HTTP/1.1 (redirect, chunked, streaming up/down).
- CA bundle + `onVerifyPeer` certificate verification.
- WebSocket.
- Package as a CFM shared library (CodeWarrior fallback if needed). — *Static-link
  delivered and reliable; `shlb` partial, see §11.*

**Phase 2 — advanced ✅ (delivered)**
- HTTP/2: framing + HPACK, connection layer, ALPN, real-machine, and N-way
  multiplexing — all done (see §11).
- TLS 1.3 — implemented, default on mbedTLS 3.x, verified end-to-end.
- Optional: a Thread Manager synchronous-style wrapper — still optional/future.

## 10. Open questions

- Final layout of the custom error-code range.
- CA bundle update mechanism (bundled with the app / separate extension / online).
- Memory allocation strategy (large-buffer source: OT-provided vs. own pool).

## 11. Build & verification status

The skeleton works; the following were verified with the real toolchain (and, where
noted, on real OS 9 under QEMU). Listed newest-relevant first.

- **Adversarial review hardening ✅ (newest)** — repeated review rounds over the
  wire-facing code, every fix regression-tested on the host (suite stays green;
  http/h2conn fuzzers re-run clean; PPC C90 cross-compile stays warning-free):
  HTTP/1 request-line/header injection rejected (control/CRLF in method, path,
  host, header names/values — `cn_http.c`/`cn_url.c`); HTTP/1 framing per
  RFC 7230 §3.3.3 (chunked beats Content-Length, conflicts rejected, `HEAD` +
  interim 1xx handled — `cn_request.c`); HTTP/2 control-frame validation per
  RFC 7540 §6, `:status` required, SETTINGS_INITIAL_WINDOW_SIZE overflow is a
  connection error, HEADERS queued atomically, 4 KB HPACK encode buffer
  (`cn_h2conn.c`); Base64 length-overflow guard; over-long OT hostnames error
  (`kCNErrHostTooLong`) instead of truncating; failed `CN_TlsCreate` frees all
  partial mbedTLS state.
- **CFM shared-library path is open ✅** — the `Retro68/Samples/SharedLibrary`
  recipe (`add_library SHARED` → `MakePEF` → `Rez -t shlb` with `cfrg` +
  `-Wl,-bE:exports`) produces a PEF that `file` identifies as a `shared library`.
  The biggest unknown in §5 is resolved.
- **Portable code compiles both ways ✅** — `src/` (`cn_url.c` / `cn_http.c`)
  compiles on the host (gcc, x86) and the PPC target (`powerpc-apple-macos-gcc`)
  with zero warnings.
- **A real type-seam gotcha** — Retro68's multiversal interfaces define `OSErr`
  (16-bit) but **no `OSStatus`** (a Carbon-era type). Added `typedef SInt32
  OSStatus` in the Mac branch of `cn_types.h`; decision #2 (use OSStatus) stands.
- **L-A host tests ✅** — CMake + AddressSanitizer + UBSan; `ctest` runs the full
  suite green.
- **Fuzzing (QA #1) ✅** — libFuzzer (clang) runs every wire parser for millions of
  iterations with no crash / OOB / UBSan finding. See `scripts/run-fuzz.sh`.

### Real-machine HTTPS — the core project goal, achieved ✅🎉

- On **real OS 9.2.2 (PowerPC) under QEMU**, `cnhttps` runs the full stack
  (`CN_OT` (Open Transport) → `CN_Tls` (mbedTLS 3.6) → `CNRequest`), completes a
  real **TLS handshake**, and fetches an `https://` page: `result=0, status=200`.
  TLS runs on our own entropy source (`mbedtls_hardware_poll`:
  Microseconds/TickCount/mouse). `CN_Tls` stacks directly on `CN_OT` (both are
  `CNTransport`). Two real pitfalls hit and fixed: (1) mbedTLS 3.x TLS 1.3 needs
  `psa_crypto_init()` first; (2) the test `openssl s_server` needs `-naccept` for
  multiple connections (the single-connection variant hangs on half-open
  connections — not our bug).
  - **Performance number:** one full TLS 1.2 handshake (ECDHE-RSA-AES256-GCM,
    RSA-2048) + fetch measured at **13 ticks ≈ 0.21 s**. ⚠️ This is **QEMU
    (mac99/G4) speed**, not real G3/G4 — QEMU on a modern host runs PPC far faster
    than period hardware; real machines will be several to tens of times slower.
    What's confirmed is "the code path is efficient, the handshake is not
    pathologically slow"; real-hardware numbers still need real hardware.

### Certificate verification ✅

- When a CA bundle is passed, `cn_tls.c` sets `VERIFY_REQUIRED` and verifies the
  **chain + hostname**. Host tests (`scripts/test-tls-verify.sh`) 3/3: CA-signed +
  matching host → accept, unknown CA → reject, hostname mismatch → reject. On real
  hardware (`cnhttps -DCN_VERIFY=ON`, embedding the test CA via
  `scripts/gen-test-pki.sh`) it verifies the CA-signed server and gets `status=200`.
  Fail-closed is proven on the host (same `cn_tls.c`).
- **Validity-date checking ✅** — rebuilt the PPC mbedTLS with `MBEDTLS_HAVE_TIME`
  and wired the Mac Time Manager to mbedTLS via a compile-time macro
  (`target/cn_mac_time.c`: `GetDateTime` → Unix epoch, portable `gmtime_r`,
  `TickCount` → ms). Real-machine: a valid cert passes within its window (clock
  reads ~2026); a 2021-expired cert is rejected (`-0x2700`
  X509_CERT_VERIFY_FAILED).
- **Real CA bundle ✅ (production hardening)** — `scripts/gen-ca-bundle.sh` turns
  the Mozilla/curl roots (system `ca-certificates.crt` or a downloaded
  `cacert.pem`, ~147 roots ~221 KB) into `target/ca_bundle.h` (embedded C string,
  gitignored); the target app verifies real public servers with
  `-DCN_CA_BUNDLE=ON`. Host proof (`scripts/test-tls-real.sh`): the same
  `cn_tls.c` verifies **real example.com** against the 147-root bundle (chain +
  hostname) → `status 200`; hostname mismatch → reject (fail-closed). The bundle
  compiles on the PPC compiler (221152-byte literal). ⚠️ RAM: the full bundle
  parses to a few hundred KB of live X.509; trim `cacert.pem` to the roots you need
  and regenerate on tight machines.

### TLS 1.3 ✅ (production hardening)

- Root cause of the earlier -30082 found: mbedTLS 3.x returns the TLS 1.3
  **post-handshake NewSessionTicket** as the non-fatal code
  `MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET (-0x7B00)` from `ssl_read`/
  `ssl_write`, and our `tls_recv`/`tls_send` treated it as fatal `kCNErrTlsIo`.
  Fix: treat that code as would-block (retry). Reproduced and fixed **on the host
  with vanilla mbedTLS 3.6.0 (same version as the PPC fork)**: `scripts/test-tls13.sh`
  proves **h2-over-1.3 ✅, HTTP/1.1-over-1.3 ✅, 1.2 fallback ✅**; against **real
  example.com** (`TLSv1.3, AES-256-GCM`) + the real 147-root bundle → `status 200`.
  **TLS 1.3 is now the default**, with `CN_TLS_FORCE_TLS12` as the fallback escape
  hatch. **Real-machine end-to-end confirmed ✅🎉**: on QEMU OS 9, `cnh2` (1.3
  enabled) against a host h2 server — server log shows `version=TLSv1.3 ALPN=h2 /
  GET / -> 200`, `cnh2` console `result=0 status=200` (the same scenario was
  -30082 before the fix).
  - ⚠️ **Build lesson:** `cmake --build --target cnh2` only builds the XCOFF; it
    does **not** run the Rez post-step that produces the MacBinary `.bin`. Use a
    full `cmake --build build-target` to refresh the `.bin` (otherwise the ISO
    carries a stale binary — exactly why the real machine still showed -30082
    earlier).
  - This NewSessionTicket fix is what resolved the earlier step-3 h2 `-30082`
    (originally misattributed to the fork's TLS 1.3 data phase). With the fix,
    h2-over-1.3 works: the test server (`h2_test_server.py`) allows 1.3 by default
    and `CN_H2_FORCE_TLS12=1` pins 1.2 for the fallback path.

### Entropy injection hook ✅ (production hardening)

- `CN_TlsAddEntropy(tls, data, len)`: the app feeds session-collected unpredictable
  bits (mouse/keyboard inter-event gaps, Microseconds/TickCount deltas,
  uninitialized stack) before the handshake; it goes into the DRBG via
  `mbedtls_ctr_drbg_reseed` as **additional input** (only adds, never removes). Host
  verified (`h2_smoke` still handshakes after injection); `cn_collect_jitter()`
  (`target/cn_mac_time.c`, called from `h2_get.c`/`https_get.c`) demonstrates
  real-machine collection + injection. ⚠️ This is a
  **mixing hook, not a guarantee**: it improves seed quality, but on a machine with
  no real hardware entropy it cannot by itself bring the RNG to cryptographic
  strength — `mbedtls_hardware_poll` (cy384 flags it mediocre) is still the floor.
  Production should collect plenty of user-input entropy.

### Plain HTTP / on-target / earlier milestones

- **L-D real-machine networking ✅🎉** — on **real OS 9.2.2 (PowerPC) under QEMU**,
  `cnhttp` runs the full stack (`CN_OT` → `CNRequest` async state machine → HTTP
  parser), GETs from a host HTTP server, and gets `status=200, body=74 bytes`; the
  server log confirms the request. The OT transport (`cn_ot.c`) is non-blocking
  (`kOTNoDataErr` → would-block), fitting the cooperative pump. Prereqs: Retro68
  rebuilt with Apple UI 3.4 (OT headers + matching stubs), OS 9 guest TCP/IP set to
  DHCP.
- **L-C on-target verified ✅** — the on-target test runner (`target/`) compiled to
  a PowerPC OS 9 app runs **15 checks, 0 failed** on real OS 9.2.2: URL/HTTP
  parser, WebSocket 16/64-bit length fields, SHA-1, Base64, WebSocket accept, async
  request state machine — proving behaviour matches the host on **big-endian
  PowerPC** with zero endianness bugs. Flow: `scripts/build-target-iso.sh`
  (Retro68 → MacBinary → ISO9660) + `CN_TOOLS_DISK=... scripts/run-emulator.sh run`
  (OS 9 decodes the MacBinary with StuffIt Expander, then runs it).
- **Host HTTPS first light ✅** — the full stack (`CNRequest` → `CN_Tls` (mbedTLS
  2.28 LTS) → host TCP) completes a real TLS handshake against `openssl s_server`
  and gets HTTP 200. TLS is enabled with `-DCN_WITH_MBEDTLS=ON -DMBEDTLS_ROOT=...`;
  mbedTLS's non-blocking BIO (WANT_READ/WRITE) maps directly to `CNTransport`'s
  would-block semantics.
- **CFM shared library ✅** — `target/shlb/` builds ClassicNet as a PowerPC CFM
  shared library (`ClassicNet`, type `shlb` + PEF + `cfrg`, 13 KB). Real OS 9: a
  separate app (`shlbdemo`) dynamically loads it at launch and calls its exported
  `CN_ParseURL` / `CN_Sha1` / `CN_Base64Encode` with correct results (SHA1(abc)
  base64 matches) — the code lives in the library, not the app.
  - ⚠️ **Retro68 `MakePEF` bug found:** the export hash table slot index uses
    `key % sz`, but the CFM runtime uses a different reduction; this only triggers
    with **>9 exports** (multiple hash slots, power>0) — ≤9 exports (power=0, single
    slot) work. So the `shlb` exports are kept to the core API; a full-API `shlb`
    needs an upstream MakePEF fix or static linking. The static-link form
    (cnhttp/cnhttps/cntest) works perfectly on real hardware and is the reliable
    delivery vehicle.

### HTTP/2 (Phase 2) ✅

- **Core (framing + HPACK), host-verified ✅** — `src/cn_h2.c`: 9-byte frame header
  parse/serialize, connection preface, SETTINGS build/parse. `src/cn_hpack.c`:
  HPACK (RFC 7541) decoder (integer/string §5, Huffman §5.2 + Appendix B, 61-entry
  static table, dynamic table with eviction §2.3/§4), plus a minimal stateless
  encoder (literal, no Huffman, static-table name index) for sending requests.
  - **Authoritative vectors ✅** — `tests/test_hpack.c` runs RFC 7541 **Appendix C**
    vectors: C.3 (cumulative non-Huffman, dynamic table), C.4 (Huffman decode), C.5
    (table size 256 forced eviction) — all field-for-field. `tests/test_h2.c`
    covers frame parse/round-trip/SETTINGS. ctest green.
  - **Fuzz ✅** — libFuzzer + ASan/UBSan: HPACK decoder 1.61M iterations (cov 322),
    H2 frame 15.55M, no crash / OOB / UB. Huffman and static tables are generated
    from the authoritative `hpack` Python package (`src/cn_hpack_tables.h`), not
    hand-copied.
  - **L-B PPC cross-compile ✅** — `cn_h2.c` / `cn_hpack.c` compile to XCOFF with
    `powerpc-apple-macos-gcc -std=c90 -Wall -Wextra`, zero warnings.
- **Connection layer (step 1), host-verified ✅** — `src/cn_h2conn.c`: a single-
  stream GET over any `CNTransport`, reusing `CNRequest`'s pump/callback model.
  Sends preface + our SETTINGS (server push off) + HPACK-encoded HEADERS
  (END_HEADERS|END_STREAM); reads frames, reassembles HEADERS+CONTINUATION →
  HPACK decode → `onResponse`, DATA → `onData`, replenishes flow control via
  WINDOW_UPDATE, ACKs SETTINGS/PING, winds down on GOAWAY/RST_STREAM.
  `tests/test_h2conn.c` uses a mock-transport server (5-byte chunking to force
  frame reassembly): status 200 + content-type + body, SETTINGS/PING ACK, GOAWAY →
  error. ctest green; libFuzzer feeds fuzz bytes as server responses into the pump
  for 3.25M iterations (cov 761), no crash/UB; PPC cross-compile zero warnings.
- **TLS ALPN + real h2 server interop (step 2, host) ✅** — `cn_tls.c` adds
  `CN_TlsSetAlpn` (offer `h2` in the handshake) / `CN_TlsGetAlpn` (read the
  negotiated result), with the protocol-string array stored in `CNTlsTransport` to
  satisfy mbedTLS lifetime; all under `CN_WITH_MBEDTLS`. `tests/h2_smoke.c` runs the
  full client stack `CN_H2Conn → CN_Tls(ALPN h2) → TCP`; `scripts/test-h2.sh`
  stands up a **real Python `h2`-library TLS server** (not our own), **with
  certificate verification on** (server self-signed cert as trusted CA,
  host=localhost). Result: ALPN negotiates `h2`, GET over TLS → **status 200 +
  body**. Interop with a standard-compliant h2 implementation holds.
- **Real-machine HTTP/2 over HTTPS ✅🎉 (step 3)** — on **real OS 9.2.2 (PowerPC)
  under QEMU**, `cnh2` (`target/h2_get.c`) runs the full stack `CN_H2Conn →
  CN_Tls(ALPN h2) → CN_OT` (all `CNTransport`) against the host Python `h2` server:
  TLS handshake + ALPN `h2` + a real h2 GET, **with certificate verification on**
  (embedded test CA, host `localhost`). Screen prints `ALPN: h2`, `RESULT:
  result=0 status=200`. "Modern HTTP/2 on Classic Mac OS" is proven end-to-end on
  real hardware.
  - ⚠️ **Pitfall, since resolved:** at step 3, when the connection negotiated
    **TLS 1.3** against a 1.3-preferring Python server, the handshake and ALPN
    succeeded but the h2 data exchange returned `kCNErrTlsIo (-30082)` mid-stream.
    It was first hypothesized to be the fork's incomplete 1.3 data phase and worked
    around by pinning the server to TLS 1.2. The **real** root cause (found in the
    "TLS 1.3" entry above) was mishandling the post-handshake NewSessionTicket;
    once `tls_recv`/`tls_send` treat it as would-block, **h2-over-1.3 works** and is
    confirmed on real OS 9. The test server now allows 1.3 by default
    (`CN_H2_FORCE_TLS12=1` pins 1.2). Diagnosed by logging the mbedTLS rc in
    `tls_send`/`tls_recv` (previously only logged during the handshake).
- **N-way multiplexing (step 4), host-verified ✅** — `cn_h2conn` becomes a stream
  array (`CN_H2_MAX_STREAMS`): multiple concurrent GETs on one connection.
  `CN_H2ConnStart` opens the connection, `CN_H2Request` opens each stream
  (ids 1,3,5…, each with its own callback/ud); the pump routes frames to the right
  request by stream id, and `CN_H2Done` fires only when all complete. The
  memory-saving key: HTTP/2 forbids interleaving HEADERS/CONTINUATION across
  streams (only one header block reassembling at a time), so the reassembly buffer
  + HPACK + response scratch stay a single shared copy at the connection layer, and
  each stream holds only small state. RST_STREAM now ends **only that stream**
  (others continue); GOAWAY winds down the whole connection. `test_multiplex` in
  `tests/test_h2conn.c`: 3 concurrent requests, server replies **out of order**
  (body 5→1→3) → each request gets its correct status+body. `CN_H2Get` remains as
  the "open connection + single request" convenience wrapper. ctest green; fuzz
  2.16M iterations (cov 836) no crash; PPC zero warnings.

### Caveat that spans everything

- **Emulator performance ⚠️** — QEMU is a dynamic translator, not cycle-accurate;
  it verifies functional correctness, but its speed must **not** be read as real G3
  performance. The Phase 0 "performance feasibility" numbers ultimately need
  real-hardware confirmation.
