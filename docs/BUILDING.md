# Building ClassicNet

Three build flavours, from lightest to heaviest:

| Flavour | Toolchain | What it covers |
|---|---|---|
| **Host tests** | system gcc/clang | the portable protocol + crypto code, under ASan/UBSan + fuzzers |
| **Host TLS** | system gcc + mbedTLS | real HTTPS over a host socket transport |
| **On-target** | Retro68 (PPC) + mbedTLS-PPC | the OS 9 apps: `cntest`, `cnhttp`, `cnhttps` |

## 1. Host tests (no special setup)

```bash
cmake -S . -B build            # AddressSanitizer + UBSan on by default
cmake --build build
ctest --test-dir build --output-on-failure
./scripts/run-fuzz.sh http 30  # url | http | ws | chunked | base64
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
Fetches and builds (into `deps/`, gitignored):
- `deps/mbedtls-host` — vanilla mbedTLS 2.28 LTS for the host TLS test.
- `deps/mbedtls-ppc` — cy384's classic-Mac mbedTLS 3.6 fork (TLS-client config +
  `mbedtls_hardware_poll` entropy) built for PowerPC.

## 4. Host TLS test

```bash
cmake -S . -B build-tls -DCN_WITH_MBEDTLS=ON -DCN_SANITIZE=OFF \
      -DMBEDTLS_ROOT=deps/mbedtls-host
cmake --build build-tls
CN_BUILD_DIR=build-tls ./scripts/test-tls.sh   # real HTTPS GET vs openssl s_server
```

## 5. On-target apps (OS 9 / PowerPC)

```bash
TC=$RETRO68_TOOLCHAIN/powerpc-apple-macos/cmake/retroppc.toolchain.cmake
cmake -S target -B build-target -DCMAKE_TOOLCHAIN_FILE=$TC \
      -DMBEDTLS_PPC_ROOT=deps/mbedtls-ppc
cmake --build build-target
# -> build-target/{cntest,cnhttp,cnhttps}.bin  (MacBinary, PowerPC PEF)
```

## 6. Running on OS 9 under QEMU

Needs `qemu-system-ppc` and an OS 9 install ISO (user-supplied). See
`scripts/run-emulator.sh` (install/run) and `scripts/build-target-iso.sh`.
The Retro68 `.dsk` is not mountable under QEMU, so apps are shipped on an
ISO9660 CD as MacBinary and decoded in the guest with StuffIt Expander:

```bash
./scripts/build-target-iso.sh                        # cntest.iso (smoke runner)
CN_TOOLS_DISK=build-target/cntest.iso ./scripts/run-emulator.sh run
```
For `cnhttp` / `cnhttps`, run a host server (`python3 -m http.server 8080` or
`openssl s_server -accept 8443 -www -naccept 100 -no_tls1_3 ...`); the guest
reaches the host at `10.0.2.2` via QEMU user-net (configure the guest's TCP/IP
control panel for DHCP).
