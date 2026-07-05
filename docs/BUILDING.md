# Building ClassicNet

Build flavours, from lightest to heaviest:

| Flavour | Toolchain | What it covers |
|---|---|---|
| **Host tests** | system gcc/clang | the portable protocol code, under ASan/UBSan + libFuzzer |
| **Host TLS 1.2** | system gcc + mbedTLS 2.28 LTS | real HTTPS over a host socket transport; certificate-verification suite |
| **Host TLS 1.3 / h2** | system gcc + mbedTLS 3.6 | TLS 1.3 (incl. NewSessionTicket), ALPN, HTTP/2 interop vs a real `h2` server |
| **On-target** | Retro68 (PPC) + mbedTLS-PPC | the OS 9 apps: `cntest`, `cnhttp`, `cnhttps`, `cnh2` (+ the CFM `shlb` demo) |

## 1. Host tests (no special setup)

```bash
cmake -S . -B build            # AddressSanitizer + UBSan on by default
cmake --build build
ctest --test-dir build --output-on-failure   # 11 unit-test binaries
./scripts/run-fuzz.sh http 30  # url | http | ws | chunked | base64 | hpack | h2 | h2conn
```

## 2. Toolchain: Retro68 + Apple Universal Interfaces 3.4

The on-target build needs Retro68 built **with Apple's Universal Interfaces**
(the open-source "multiversal" interfaces lack Open Transport headers). One-time:

1. Build Retro68 normally (see its README) — gives a working `Retro68-build/`.
2. Get the Universal Interfaces & Libraries (the Retro68-ready bundle from
   Macintosh Garden's MPW page, `InterfacesAndLibraries.zip`,
   md5 `c28aaf23195679f9849adf12b0381f6e`).
3. Unzip and place the `Interfaces&Libraries` folder at
   `<Retro68-src>/InterfacesAndLibraries/`.
4. Rebuild just the interfaces + libraries (reuses the existing gcc):
   ```bash
   cd <Retro68-build>
   <Retro68-src>/build-toolchain.bash --skip-thirdparty
   ```
   The log should say "Using Apple's Universal Interfaces."

Set `RETRO68_TOOLCHAIN` to `<Retro68-build>/toolchain` if it isn't the default.

## 3. Dependencies: mbedTLS

```bash
./scripts/setup-mbedtls.sh
```
Fetches and builds (into `deps/`, gitignored; pinned versions, idempotent):
- `deps/mbedtls-host` — vanilla mbedTLS 2.28 LTS for the host TLS 1.2 tests.
- `deps/mbedtls-host3` — vanilla mbedTLS 3.6 (same version as the PPC fork) for
  the host TLS 1.3 tests (`scripts/test-tls13.sh`).
- `deps/mbedtls-ppc` — cy384's classic-Mac mbedTLS 3.6 fork (TLS-client config +
  `mbedtls_hardware_poll` entropy), pinned to a commit, built for PowerPC with
  the Retro68 toolchain from §2.

## 4. Host TLS tests

Against mbedTLS 2.28 (TLS 1.2):

```bash
cmake -S . -B build-tls -DCN_WITH_MBEDTLS=ON -DCN_SANITIZE=OFF \
      -DMBEDTLS_ROOT=deps/mbedtls-host
cmake --build build-tls
CN_BUILD_DIR=build-tls ./scripts/test-tls.sh  # HTTPS GET vs openssl s_server
./scripts/gen-test-pki.sh                     # once: throwaway CA + server certs
./scripts/test-tls-verify.sh                  # fail-closed verification, 3 cases
./scripts/test-tls-real.sh                    # verify a real public server vs the system roots
./scripts/test-h2.sh                          # HTTP/2 over TLS (ALPN h2) vs a real Python h2 server
./scripts/test-h2-post.sh                     # HTTP/2 POST, body echoed back
```

Against mbedTLS 3.6 — TLS 1.3, including the post-handshake NewSessionTicket
path (configures and builds `build-tls3` itself):

```bash
./scripts/test-tls13.sh   # h2-over-1.3, HTTP/1.1-over-1.3, 1.2 fallback
```

## 5. On-target apps (OS 9 / PowerPC)

```bash
TC=$RETRO68_TOOLCHAIN/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
cmake -S target -B build-target -DCMAKE_TOOLCHAIN_FILE=$TC \
      -DMBEDTLS_PPC_ROOT=deps/mbedtls-ppc
cmake --build build-target
# -> build-target/{cntest,cnhttp,cnhttps,cnh2}.bin  (MacBinary, PowerPC PEF)
```

Configure-time options:

| Option | Effect |
|---|---|
| `-DCN_VERIFY=ON` | require certificate verification against the throwaway test CA (`scripts/gen-test-pki.sh` → `target/test_ca.h`) |
| `-DCN_CA_BUNDLE=ON` | verify against the embedded Mozilla root bundle (`scripts/gen-ca-bundle.sh` → `target/ca_bundle.h`); takes precedence over `CN_VERIFY` |
| `-DCN_LAUNCHAPPL=ON` | headless build for LaunchAPPL push-to-run (skips the "Press Return to quit" pause) |

With neither CA option, `cnhttps`/`cnh2` run with verification **off** and print
`verify: NONE (insecure)` — development only, never ship that.

⚠️ Building a single target (`cmake --build build-target --target cnh2`) only
refreshes the XCOFF, **not** the MacBinary `.bin` (a Rez post-build step). Run a
full `cmake --build build-target` before packaging, or the ISO carries a stale
binary.

The CFM shared-library demo builds separately:

```bash
cmake -S target/shlb -B build-shlb -DCMAKE_TOOLCHAIN_FILE=$TC
cmake --build build-shlb   # -> ClassicNet.bin (the shlb) + shlbdemo.bin
```

## 6. Running on OS 9 under QEMU

Needs `qemu-system-ppc` and an OS 9 install ISO (user-supplied). See
`scripts/run-emulator.sh` (install/run). Two ways to get apps into the guest:

**CD image** — the Retro68 `.dsk` is not mountable under QEMU, so apps ship on
an ISO9660 CD as MacBinary and are decoded in the guest with StuffIt Expander:

```bash
./scripts/build-target-iso.sh                        # cntest.iso (smoke runner)
CN_TOOLS_DISK=build-target/cntest.iso ./scripts/run-emulator.sh run
./scripts/swap-iso.sh <new.iso>                      # hot-swap the CD, no reboot
```

**Push-to-run (no reboot, no CD)** — run LaunchAPPLServer in the guest
(`scripts/build-launchappl-iso.sh` ships it; set it to "OpenTransport TCP",
port 1984), then:

```bash
./scripts/launch-app.sh build-target/cntest.bin   # push, run, stream stdout back
```

For the networked apps, run a host server; the guest reaches the host at
`10.0.2.2` via QEMU user-net (set the guest's TCP/IP control panel to DHCP):

- `cnhttp`: `python3 -m http.server 8080`
- `cnhttps`: `openssl s_server -accept 8443 -cert deps/test-pki/srv.pem -key deps/test-pki/srv-key.pem -www -naccept 100`
- `cnh2`: `./scripts/run-h2-server.sh` (Python `h2` server on 8444, test PKI)
