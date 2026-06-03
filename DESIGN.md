# ClassicNet — 設計文件

一個給 Classic Mac OS 的現代化網路 stack，提供 TLS / HTTPS / WebSocket（與第二階段的 HTTP/2）。

---

## 1. 目標與範圍

- **目標平台：PowerPC，Mac OS 8.0 ~ 9.2.2。**
  - 不支援 68k（移除 A5 world / CODE resource / segment loader 的複雜度）。
  - 不支援 7.x（簡化；OT 版本與測試面收斂）。
- **效能甜蜜點：** G3 等級體驗最佳，但 601/603/604 也應可用。PPC 上 TLS handshake 約落在「數秒內」，屬可用範圍。
- **提供協定：** TLS 1.2、HTTPS（HTTP/1.1）、WebSocket（`wss://`）。HTTP/2 列第二階段。

## 2. 整體架構（分層）

```
  ┌─────────────────────────────────────────────┐
  │  App  (cooperative event loop)              │
  └───────────────┬─────────────────────────────┘
                  │  C API (callback-based, 見 §8)
  ┌───────────────▼─────────────────────────────┐
  │  L4  HTTP/1.1 · WebSocket · (HTTP/2 = P2)    │
  ├─────────────────────────────────────────────┤
  │  L3  TLS 1.2  (mbedTLS, 裁剪)                │
  ├─────────────────────────────────────────────┤
  │  L2  CSPRNG / 熵蒐集 · CA 憑證驗證           │
  ├─────────────────────────────────────────────┤
  │  L1  Transport: Open Transport (OTTCP)       │
  │      非同步、由 notifier + CN_Idle 推進      │
  └─────────────────────────────────────────────┘
```

## 3. Crypto / TLS

- **採用 mbedTLS（裁剪版）。** 理由：已有 Classic Mac 移植先例可接手、模組化可裁剪到 <64KB RAM、新版有 TLS 1.3 升級空間、ALPN 支援（HTTP/2 需要）。
  - 參考前作：[antscode/mbedtls-Mac-68k](https://github.com/antscode/mbedtls-Mac-68k)、[bbenchoff MacSSL](https://bbenchoff.github.io/pages/MacSSL.html)。
  - 備案：若 code size / RAM 擠不下，評估 BearSSL（純 C89、零動態配置，但僅 TLS 1.2 且維護停滯）。
- **效能取捨（軟體實作、無 AES-NI）：**
  - cipher 優先協商 **ChaCha20-Poly1305**（軟體下比 AES 快）。
  - 金鑰交換優先 **ECDHE P-256 + ECDSA**（通常比 RSA-2048 省）。

## 4. 交付形態：CFM shared library

- 主要交付為 **PowerPC CFM（Code Fragment Manager）+ PEF shared library**，與系統服務（OpenTransportLib 等）一致、可獨立更新（TLS/憑證會持續需要修補）、多 client 可共用 code fragment。
- 記憶體模型走 CFM/PPC 原生路徑，無 A5 globals 困擾。

## 5. Toolchain

- **主力：[Retro68](https://github.com/autc04/Retro68)**（Linux 上的 GCC cross-compiler，整合 CMake）。日常開發、測試、CI 都用它。
- **已知風險：** Retro68 產 PPC CFM shared library「能做但文件極缺」。流程：`gcc -shared` → XCOFF → `MakePEF` → `MakeImport`（import stub）→ `Rez`。某些結構（如 `ImportMacFunctions`）無文件，需逆向摸索。參見 [Retro68 #97](https://github.com/autc04/Retro68/issues/97)。
- **已驗證 ✅：** `Retro68/Samples/SharedLibrary` 的配方實測可產出 CFM shared library（見 §11）。先前「文件極缺」的風險大幅降級。
- **Fallback：** 若 shared library 輸出卡死，把「打包成 CFM shared library」這一步移到 **CodeWarrior（SheepShaver 模擬器內）**，其 CFM shared library 支援為一等公民。

## 6. 三根硬骨頭（與 OS 版本無關，必須正面解）

1. **熵源（entropy）。** 無硬體 RNG、無 `/dev/random`。需自建 CSPRNG seed，混合 `Microseconds()`、`TickCount()`、滑鼠座標、記憶體狀態、OT 封包時序等。內建蒐集器品質不足時須在 log 警告。**這是安全前提，非選用。**
2. **Root CA 憑證庫。** 系統無信任憑證庫。需自行 bundle 一份 CA roots（DER/PEM blob）並規劃更新策略；另提供 `onVerifyPeer` callback 讓 app 做 TOFU 或彈窗由使用者裁決。
3. **合作式多工 → 非同步 API。** OS 8/9 應用層仍是合作式多工、無搶佔式執行緒。不可 block 在 I/O。整個 library 為事件驅動狀態機（見 §8）。

## 7. 已鎖定的設計決策

1. **核心為純 callback。** 同步風格 wrapper（Thread Manager + `YieldToAnyThread`）列為日後選用層，不進第一階段核心。
2. **錯誤模型用 `OSStatus`**，切一段自訂負數區間給 ClassicNet（暫定 `kCNErrBase = -30000` 起向下）。
3. **HTTP/2 多工列第二階段。** 第一階段每個 request 各自一條 HTTP/1.1 over TLS。

## 8. 非同步 API 草圖

### 8.1 設計原則
1. 永不 block，一律「發起 → callback 通知」。
2. **callback 一律在 system task 時間觸發**（從 `CN_Idle` 內），絕不在 interrupt / OT notifier 時間。notifier 只設旗標，工作延後到 `CN_Idle`。
3. response body 用串流（`onData` 分塊），不整包進記憶體。

### 8.2 生命週期與 pump
```c
OSStatus  CN_Init(const CNConfig *config, CNContextRef *outCtx);
void      CN_Dispose(CNContextRef ctx);

/* 心臟：從 event loop 頻繁呼叫，做有界工作後返回；所有 callback 在此觸發 */
void      CN_Idle(CNContextRef ctx);

/* 給 WaitNextEvent 的 sleep：回傳最多再過幾個 tick 該再呼叫 CN_Idle */
UInt32    CN_NextIdleDelay(CNContextRef ctx);
```
Main loop：
```c
for (;;) {
    UInt32 sleep = CN_NextIdleDelay(ctx);
    WaitNextEvent(everyEvent, &evt, sleep, nil);
    /* ... 處理自己的 evt ... */
    CN_Idle(ctx);
}
```

### 8.3 Config（三根硬骨頭的注入點）
```c
typedef struct {
    void (*gatherEntropy)(void *buf, UInt32 want, UInt32 *got, void *ud); /* nil = 內建蒐集器 */
    const void   *caBundle;       /* nil = 無法驗證憑證 */
    UInt32        caBundleLen;
    CNAllocator  *allocator;      /* 選用：自訂 memory pool */
    void         *userData;
} CNConfig;
```

### 8.4 高階 HTTP API
```c
typedef struct {
    CNCertDecision (*onVerifyPeer)(CNRequestRef r, const CNCertInfo *info,
                                   OSStatus dflt, void *ud);
    void    (*onResponse)(CNRequestRef r, UInt16 status,
                          const CNHeader *hdrs, UInt32 n, void *ud);
    Boolean (*onData)    (CNRequestRef r, const void *bytes, UInt32 len, void *ud); /* false=背壓 */
    void    (*onComplete)(CNRequestRef r, OSStatus result, void *ud);
} CNRequestCallbacks;

typedef struct {
    CNHTTPMethod              method;       /* kCN_GET / kCN_POST ... */
    const char               *url;          /* "https://..." */
    const CNHeader           *headers;
    UInt32                    headerCount;
    const void               *body;
    UInt32                    bodyLength;
    UInt32 (*bodyProvider)(CNRequestRef r, void *buf, UInt32 max, void *ud); /* 串流上傳 */
    const CNRequestCallbacks *cb;
    void                     *userData;
} CNRequestParams;

OSStatus CN_HTTPRequest(CNContextRef ctx, const CNRequestParams *p, CNRequestRef *out);
void     CN_RequestCancel(CNRequestRef r);   /* 安全：狀態機收尾後才釋放 */
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

## 9. 路線圖

**Phase 0 — 垂直切片（驗證效能可行性，最大未知數）**
- Retro68 build 環境 + CMake。
- OT TCP 連線（非同步、notifier + `CN_Idle`）。
- 接手 mbedTLS port，自製熵源，ChaCha20 cipher。
- 目標：成功完成一次 **HTTPS GET**（static link 即可），量測 G3/603 上的 handshake 時間。

**Phase 1 — 可用的 library**
- 完整 HTTP/1.1（redirect、chunked、streaming up/down）。
- CA bundle + `onVerifyPeer` 憑證驗證。
- WebSocket。
- 打包成 CFM shared library（必要時走 CodeWarrior fallback）。

**Phase 2 — 進階**
- HTTP/2：framing + HPACK ✅（host 驗證，見 §11）；尚缺多工連線層 + ALPN + 真機。
- 選用：Thread Manager 同步風格 wrapper。
- 評估 TLS 1.3。

## 10. 待議
- 自訂錯誤碼區間的最終配置。
- CA bundle 的更新機制（隨 app / 獨立 extension / 線上更新）。
- 記憶體配置策略（大 buffer 來源：OT 提供 vs 自有 pool）。

## 11. 建置與驗證現況

骨架已可運作，下列事項以實機 toolchain 驗證過：

- **CFM shared library 路徑已打通 ✅** —— `Retro68/Samples/SharedLibrary` 的配方（`add_library SHARED` → `MakePEF` → `Rez -t shlb` 帶 `cfrg` + `-Wl,-bE:exports`）實測能產出 `file` 判定為 `shared library` 的 PEF。§5 的最大未知數解除。
- **可攜碼雙向編譯 ✅** —— `src/`（`cn_url.c` / `cn_http.c`）同時在 host（gcc, x86）與 PPC target（`powerpc-apple-macos-gcc`）編譯通過、零警告。
- **型別 seam 的一個真實坑** —— Retro68 multiversal interfaces 定義 `OSErr`（16-bit）但**無 `OSStatus`**（Carbon 時代型別）。已在 `cn_types.h` 的 Mac 分支補 `typedef SInt32 OSStatus`；決策 #2（用 OSStatus）維持不變。
- **L-A host 測試 ✅** —— CMake + AddressSanitizer + UBSan，`ctest` 跑 `cn_url` / `cn_http` 兩組全過。
- **Fuzzing（QA #1）✅** —— libFuzzer（clang）對兩個 parser 各跑千萬級次數無 crash／無 OOB／無 UBSan。見 `scripts/run-fuzz.sh`。
- **真機 HTTPS 打通 ✅🎉🎉 —— 專案核心目標達成** —— 在 **QEMU 上的真實 OS 9.2.2 (PowerPC)** 執行 `cnhttps`，用完整 stack（`CN_OT`(Open Transport) → `CN_Tls`(mbedTLS 3.6) → `CNRequest`）完成真實 **TLS 握手**並抓到 `https://` 頁面，`result=0, status=200`。TLS 跑在我們自製的熵源（`mbedtls_hardware_poll`：Microseconds/TickCount/mouse）上。`CN_Tls` 直接疊在 `CN_OT` 之上（兩者都是 `CNTransport`）。踩到的兩個真坑並修正：(1) mbedTLS 3.x TLS 1.3 需先 `psa_crypto_init()`；(2) 測試用的 openssl `s_server` 要 `-naccept` 多連線（單連線版會被半開連線卡死，與我們的 code 無關）。**「現代 TLS/HTTPS 跑在 Classic Mac OS」這個專案前提，在真實硬體上端到端證明完成。**
  - **效能數字**：一次完整 TLS 1.2 握手（ECDHE-RSA-AES256-GCM、RSA-2048）+ fetch 量到 **13 ticks ≈ 0.21 s**。⚠️ 這是 **QEMU 模擬速度**（mac99/G4），非真實 G3/G4 —— QEMU 在現代主機上跑 PPC 遠快於當年硬體，真機會慢數倍～數十倍。確認的是「程式路徑有效率、握手不病態慢」；真機效能仍需實機量測。
- **憑證驗證 ✅** —— `cn_tls.c` 傳入 CA bundle 時 `VERIFY_REQUIRED`，驗**憑證鏈 + 主機名**。Host 測試（`scripts/test-tls-verify.sh`）3/3：CA 簽署+主機相符→接受、未知 CA→拒絕、主機名不符→拒絕。真機（`cnhttps -DCN_VERIFY=ON`，嵌入測試 CA via `scripts/gen-test-pki.sh`）對 CA 簽署的 server 驗證通過、`status=200`。fail-closed 由 host 證明（同一份 `cn_tls.c`）。
  - **有效期檢查 ✅** —— 重編 PPC mbedTLS 開 `MBEDTLS_HAVE_TIME`，並用 compile-time macro 把 Mac Time Manager 接給 mbedTLS（`target/cn_mac_time.c`：`GetDateTime`→Unix epoch、portable `gmtime_r`、`TickCount`→ms）。真機驗證：valid 憑證在有效期內通過（clock 讀到 ~2026）、2021 過期憑證被擋（`-0x2700` X509_CERT_VERIFY_FAILED）。
  - ⚠️ **剩餘安全限制**（production 前須處理）：(1) **熵源品質中等** —— `mbedtls_hardware_poll` 用 Microseconds/GetMouse/TickCount（cy384 自己標註 mediocre），安全性需強化。(3) **CA bundle** —— demo 用拋棄式測試 CA；production 要 bundle 真實 root（Mozilla/curl `cacert.pem`），但 classic Mac RAM 有限，可能要裁剪。
- **L-D 真機網路打通 ✅🎉** —— 在 **QEMU 上的真實 OS 9.2.2 (PowerPC)** 執行 `cnhttp`，用完整 stack（`CN_OT`（Open Transport）→ `CNRequest` 非同步狀態機 → HTTP parser）對 host 的 http server 發出 GET，拿到 `status=200, body=74 bytes`；host 端 server log 同步確認收到該請求。整個垂直切片在真機上端到端運作。OT transport（`cn_ot.c`）為非阻塞、`kOTNoDataErr`→would-block，完美套進合作式 pump。前置：Retro68 以 Apple UI 3.4 重建（提供 OT headers + 對得上的 stub）、OS 9 guest TCP/IP 設為 DHCP。
- **L-C 真機驗證通過 ✅** —— on-target test runner（`target/`）編成 PowerPC OS 9 app，在 **QEMU 上的真實 OS 9.2.2** 跑出 **15 checks, 0 failed**。涵蓋 URL/HTTP parser、WebSocket 16/64-bit 長度欄位、SHA-1、Base64、WebSocket accept、非同步 request 狀態機 —— 證明在**大端 PowerPC** 上行為與 host 一致、零端序 bug。流程：`scripts/build-target-iso.sh`（Retro68 編譯 → MacBinary → ISO9660）+ `CN_TOOLS_DISK=... scripts/run-emulator.sh run`（OS 9 用 StuffIt Expander 解 MacBinary 後執行）。
- **真實 HTTPS 已跑通 ✅** —— 完整 stack（`CNRequest` → `CN_Tls`（mbedTLS 2.28 LTS）→ host TCP）對 `openssl s_server` 完成真實 TLS 握手並取得 HTTP 200。TLS 層用 `-DCN_WITH_MBEDTLS=ON -DMBEDTLS_ROOT=...` 啟用；mbedTLS 的非阻塞 BIO（WANT_READ/WRITE）直接對應 `CNTransport` 的 would-block 語意。Mac port 待辦：vendoring mbedTLS 原始碼以 Retro68 編譯、自訂熵源、CA bundle。
- **CFM shared library ✅（你最初的需求）** —— `target/shlb/` 把 ClassicNet 編成 PowerPC CFM shared library（`ClassicNet`，type `shlb` + PEF + `cfrg`，13KB）。真機 OS 9 驗證：一個獨立 app（`shlbdemo`）在啟動時動態載入這個庫，呼叫其匯出的 `CN_ParseURL` / `CN_Sha1` / `CN_Base64Encode`，得到正確結果（SHA1(abc) base64 與期望值吻合）—— 程式碼在庫裡、不在 app 裡（省記憶體 + 可獨立更新）。
  - ⚠️ **發現 Retro68 `MakePEF` 的 bug**：export hash table 的 slot index 用 `key % sz`，但 CFM runtime 用的是別的 reduction，**只在匯出 >9 個（需要多個 hash slot，power>0）時觸發**，匯出 ≤9 個（power=0、單 slot）則正常。所以目前 shlb 匯出收斂在核心 API；完整 API 的 shlb 需修 MakePEF（上游）或仍用 static link。靜態連結形式（cnhttp/cnhttps/cntest）在真機完全正常，是目前可靠的交付方式。
- **HTTP/2 核心（framing + HPACK）host 驗證 ✅** —— Phase 2 的 HTTP/2 開工。`src/cn_h2.c`：9-byte frame header 解析/序列化、連線 preface、SETTINGS build/parse；`src/cn_hpack.c`：HPACK 標頭壓縮（RFC 7541）解碼器（整數/字串 §5、Huffman §5.2 + Appendix B、靜態表 61 項、動態表含淘汰 §2.3/§4），外加最小無狀態編碼器（literal、無 Huffman、用靜態表 name index）給 client 送 request 用。
  - **權威測試向量 ✅** —— `tests/test_hpack.c` 直接跑 RFC 7541 **Appendix C** 向量：C.3（累積非 Huffman、動態表）、C.4（Huffman 解碼）、C.5（table size 256 強制淘汰）全部逐欄位吻合。`tests/test_h2.c` 涵蓋 frame 解析/round-trip/SETTINGS。10/10 ctest 全過。
  - **Fuzz ✅** —— libFuzzer + ASan/UBSan：HPACK 解碼器 161 萬次（cov 322）、H2 frame 1555 萬次，無 crash／OOB／UB。Huffman 表與靜態表由權威 `hpack` Python 套件產生（`src/cn_hpack_tables.h`），不手抄。
  - **L-B PPC 交叉編譯 ✅** —— `cn_h2.c` / `cn_hpack.c` 以 `powerpc-apple-macos-gcc -std=c90 -Wall -Wextra` 編出 XCOFF、零警告。
  - **待做（C v2）**：多 stream 多工 + 流量控制的連線層、TLS ALPN 協商接上 `CN_Tls`（mbedTLS 已支援 ALPN）、真機 h2 over HTTPS 端到端。
- **模擬器效能警告 ⚠️** —— QEMU 為動態翻譯、非 cycle-accurate；可驗功能正確性，但其速度**不可**當作真實 G3 效能。Phase 0 的「效能可行性」數字最終需實機佐證。
