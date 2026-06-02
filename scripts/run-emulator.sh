#!/usr/bin/env bash
#
# ClassicNet -- QEMU launcher for the Mac OS 9 test target.
#
# 用法:
#   scripts/run-emulator.sh install   # 第一次：從 ISO 安裝 OS 9 到 qcow2 虛擬硬碟
#   scripts/run-emulator.sh run       # 之後：從已安裝的硬碟開機（預設）
#
# 可用環境變數（皆有預設值）:
#   CN_OS9_ISO        OS 9 安裝光碟映像路徑（install 模式必需）
#   CN_OS9_DISK       qcow2 虛擬硬碟路徑
#   CN_OS9_DISK_SIZE  虛擬硬碟大小（預設 8G）
#   CN_OS9_RAM        記憶體 MB（預設 512；切勿 >=1024，OS 9 會變不穩）
#   CN_MACHINE        qemu 機種（預設 mac99,via=pmu；替代：g3beige）
#   CN_CPU            CPU（預設 g4；搭配 g3beige 時用 g3）
#
# 備註：mac99 + sungem 是 OS 9 上網路最可靠的組合，故設為預設。
#       QEMU 是動態翻譯、非 cycle-accurate —— 可驗功能，不可信其速度當實機效能。
#
set -euo pipefail

ISO="${CN_OS9_ISO:-$HOME/Downloads/Mac_OS_9.2.2_Universal_Install.iso}"
DISK="${CN_OS9_DISK:-$HOME/.classicnet/macos9.qcow2}"
DISK_SIZE="${CN_OS9_DISK_SIZE:-8G}"
RAM="${CN_OS9_RAM:-512}"
MACHINE="${CN_MACHINE:-mac99,via=pmu}"
CPU="${CN_CPU:-g4}"
MODE="${1:-run}"

QEMU=qemu-system-ppc

# --- 前置檢查：模擬器是否安裝 ---
if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "找不到 $QEMU。請先安裝（需要 sudo）："
    echo "    sudo apt install -y qemu-system-ppc"
    echo "在本 session 可直接輸入：  ! sudo apt install -y qemu-system-ppc"
    exit 1
fi

# --- 建立虛擬硬碟（首次） ---
mkdir -p "$(dirname "$DISK")"
if [ ! -f "$DISK" ]; then
    echo "建立虛擬硬碟 $DISK（$DISK_SIZE）..."
    qemu-img create -f qcow2 "$DISK" "$DISK_SIZE"
fi

ARGS=(
    -M "$MACHINE"
    -cpu "$CPU"
    -m "$RAM"
    -drive "file=$DISK,format=qcow2,media=disk"
    -g 1024x768x32
    -device usb-kbd -device usb-mouse
    -net nic,model=sungem -net user
)

case "$MODE" in
    install)
        if [ ! -f "$ISO" ]; then
            echo "找不到安裝映像：$ISO"
            echo "請設定 CN_OS9_ISO 指向你下載的 Mac_OS_9.2.2_Universal_Install.iso"
            exit 1
        fi
        echo "ISO SHA1: $(sha1sum "$ISO" | awk '{print $1}')"
        echo "  預期之一: 5df0eecf3425cfac5afb605f5ef3b1485c39bb65 (Universal_Install)"
        echo "         或: 7054345676d0c6b9ecfcf6630d1aa92347f1e06e (Unsupported_G4s)"
        ARGS+=( -drive "file=$ISO,format=raw,media=cdrom" -boot d )
        echo ">> 安裝模式：進系統後用 Drive Setup 初始化虛擬硬碟，再執行 Mac OS 安裝程式。"
        ;;
    run)
        ARGS+=( -boot c )
        # 可選：附掛一個 HFS 工具碟（當成 CD-ROM，OS 9 較容易掛載），
        # 例如 Retro68 產出的 cntest.dsk，用來在真機上跑 on-target 測試。
        if [ -n "${CN_TOOLS_DISK:-}" ]; then
            if [ -f "$CN_TOOLS_DISK" ]; then
                echo "附掛工具碟（CD）: $CN_TOOLS_DISK"
                ARGS+=( -drive "file=$CN_TOOLS_DISK,format=raw,media=cdrom" )
            else
                echo "警告：CN_TOOLS_DISK 不存在：$CN_TOOLS_DISK"
            fi
        fi
        ;;
    *)
        echo "用法: $0 [install|run]"
        exit 1
        ;;
esac

echo "+ $QEMU ${ARGS[*]}"
exec "$QEMU" "${ARGS[@]}"
