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
- HTTP/2（framing + HPACK + 同 host 多工，對高階 API 透明）。
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
- **模擬器效能警告 ⚠️** —— QEMU 為動態翻譯、非 cycle-accurate；可驗功能正確性，但其速度**不可**當作真實 G3 效能。Phase 0 的「效能可行性」數字最終需實機佐證。
