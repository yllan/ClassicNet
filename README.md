# ClassicNet

現代化網路 stack for Classic Mac OS（PowerPC, Mac OS 8/9）—— TLS 1.3 / HTTPS / HTTP/2（多工 + 流量控制）/ WebSocket。設計細節見 [DESIGN.md](DESIGN.md)。

## 專案結構

```
include/classicnet/   公開標頭（可攜型別 seam、錯誤碼、各協定 API）
src/                  可攜協定程式碼（host 與 Mac target 共用）
tests/                host-only 單元測試（L-A，見 DESIGN.md 測試計畫）
scripts/              工具（QEMU 啟動器等）
```

## Host 單元測試（L-A）

協定邏輯寫成可攜 C，在 Linux/macOS 上以 sanitizer 編譯測試 —— 這些工具在 OS 9 上不存在，所以記憶體安全要在 host 端把關。

```bash
cmake -S . -B build          # 預設開 AddressSanitizer + UBSan
cmake --build build
ctest --test-dir build --output-on-failure
```

## Mac OS 9 模擬器（功能測試靶機，L-C/L-D）

需要 `qemu-system-ppc`（`sudo apt install -y qemu-system-ppc`）與一份 OS 9 安裝 ISO。

```bash
CN_OS9_ISO=~/Downloads/Mac_OS_9.2.2_Universal_Install.iso \
    scripts/run-emulator.sh install     # 首次安裝
scripts/run-emulator.sh run             # 之後開機
```

⚠️ QEMU 非 cycle-accurate：可驗證功能正確性，但跑出來的速度**不代表**真實 G3 效能。

## License

Apache-2.0 — 見 [LICENSE](LICENSE) 與 [NOTICE](NOTICE)。© 2026 Yung-Luen Lan。
mbedTLS / Retro68 等相依套件於 build 時取得、不隨本 repo 散布（見 NOTICE）。
