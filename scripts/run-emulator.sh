#!/usr/bin/env bash
#
# ClassicNet -- QEMU launcher for the Mac OS 9 test target.
#
# Usage:
#   scripts/run-emulator.sh install   # first time: install OS 9 from ISO onto a qcow2 disk
#   scripts/run-emulator.sh run       # afterwards: boot from the installed disk (default)
#
# Environment variables (all have defaults):
#   CN_OS9_ISO        path to the OS 9 install CD image (required for install mode)
#   CN_OS9_DISK       path to the qcow2 virtual disk
#   CN_OS9_DISK_SIZE  virtual disk size (default 8G)
#   CN_OS9_RAM        RAM in MB (default 512; never >=1024, OS 9 becomes unstable)
#   CN_MACHINE        qemu machine type (default mac99,via=pmu; alternative: g3beige)
#   CN_CPU            CPU (default g4; use g3 with g3beige)
#
# Notes: mac99 + sungem is the most reliable networking combo for OS 9, hence the
#        default. QEMU is a dynamic translator, not cycle-accurate -- it verifies
#        functionality; never read its speed as real-hardware performance.
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

# --- preflight: is the emulator installed? ---
if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "$QEMU not found. Install it first (needs sudo):"
    echo "    sudo apt install -y qemu-system-ppc"
    exit 1
fi

# --- create the virtual disk (first run) ---
mkdir -p "$(dirname "$DISK")"
if [ ! -f "$DISK" ]; then
    echo "creating virtual disk $DISK ($DISK_SIZE)..."
    qemu-img create -f qcow2 "$DISK" "$DISK_SIZE"
fi

ARGS=(
    -M "$MACHINE"
    -cpu "$CPU"
    -m "$RAM"
    -drive "file=$DISK,format=qcow2,media=disk"
    -g 1024x768x32
    -device usb-kbd -device usb-mouse
    # user-net NIC: outbound NAT (guest reaches host at 10.0.2.2) + a host->guest
    # forward to the in-guest LaunchAPPLServer (TCP 1984) for push-to-run testing.
    -nic "user,model=sungem,hostfwd=tcp:127.0.0.1:1984-:1984"
    # control socket for hot-swapping the tools CD without rebooting
    -monitor "unix:$(dirname "$DISK")/monitor.sock,server,nowait"
)

case "$MODE" in
    install)
        if [ ! -f "$ISO" ]; then
            echo "install image not found: $ISO"
            echo "set CN_OS9_ISO to your downloaded Mac_OS_9.2.2_Universal_Install.iso"
            exit 1
        fi
        echo "ISO SHA1: $(sha1sum "$ISO" | awk '{print $1}')"
        echo "  expected one of: 5df0eecf3425cfac5afb605f5ef3b1485c39bb65 (Universal_Install)"
        echo "               or: 7054345676d0c6b9ecfcf6630d1aa92347f1e06e (Unsupported_G4s)"
        ARGS+=( -drive "file=$ISO,format=raw,media=cdrom" -boot d )
        echo ">> install mode: once booted, initialize the virtual disk with Drive Setup, then run the Mac OS installer."
        ;;
    run)
        ARGS+=( -boot c )
        # Optional: attach an HFS tools disk (as CD-ROM, which OS 9 mounts most
        # readily), e.g. the Retro68-produced cntest.iso, to run on-target tests.
        if [ -n "${CN_TOOLS_DISK:-}" ]; then
            if [ -f "$CN_TOOLS_DISK" ]; then
                echo "attaching tools disk (CD): $CN_TOOLS_DISK"
                ARGS+=( -drive "file=$CN_TOOLS_DISK,format=raw,media=cdrom,id=toolscd" )
            else
                echo "warning: CN_TOOLS_DISK does not exist: $CN_TOOLS_DISK"
            fi
        fi
        # Optional: attach a writable disk (CN_XFER_DISK) to move files out of
        # the guest (e.g. an app edited in ResEdit) back to the host. On first
        # attach OS 9 reports it unreadable and offers to initialize -> format
        # as "Mac OS Standard (HFS)" so the host can read it.
        if [ -n "${CN_XFER_DISK:-}" ]; then
            echo "attaching transfer disk (writable): $CN_XFER_DISK"
            ARGS+=( -drive "file=$CN_XFER_DISK,format=raw,media=disk,id=xferdisk" )
        fi
        ;;
    *)
        echo "usage: $0 [install|run]"
        exit 1
        ;;
esac

echo "+ $QEMU ${ARGS[*]}"
exec "$QEMU" "${ARGS[@]}"
