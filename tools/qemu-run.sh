#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════════
# Helios OS — QEMU Launch Script
# ═══════════════════════════════════════════════════════════════════════════════
# Usage:
#   ./qemu-run.sh <image>            Run normally
#   ./qemu-run.sh <image> --debug    Run with GDB stub on port 1234
# ═══════════════════════════════════════════════════════════════════════════════

set -e

IMAGE=$1
DEBUG_FLAGS=""

if [[ -z "$IMAGE" ]]; then
    echo "Usage: $0 <disk-image> [--debug]"
    exit 1
fi

if [[ ! -f "$IMAGE" ]]; then
    echo "Error: Disk image '$IMAGE' not found."
    exit 1
fi

if [[ "$2" == "--debug" ]]; then
    DEBUG_FLAGS="-s -S"  # GDB stub on port 1234, wait for connection
    echo "  GDB mode: Waiting for connection on localhost:1234"
    echo "  Connect with: gdb -ex 'target remote :1234'"
fi

# ── Locate OVMF firmware ───────────────────────────────────────────────────
OVMF=""
for candidate in \
    /usr/share/OVMF/OVMF_CODE.fd \
    /usr/share/ovmf/OVMF.fd \
    /usr/share/edk2/ovmf/OVMF_CODE.fd \
    /usr/share/qemu/OVMF.fd \
    /usr/share/edk2-ovmf/x64/OVMF_CODE.fd; do
    if [[ -f "$candidate" ]]; then
        OVMF="$candidate"
        break
    fi
done

if [[ -z "$OVMF" ]]; then
    echo "Error: OVMF firmware not found."
    echo "Install with: sudo apt install ovmf"
    exit 1
fi

echo "  OVMF:  $OVMF"
echo "  Image: $IMAGE"
echo ""

# ── Launch QEMU ────────────────────────────────────────────────────────────
# Use KVM if available, fall back to TCG
ACCEL="tcg"
if [[ -e /dev/kvm ]]; then
    ACCEL="kvm"
fi

exec qemu-system-x86_64 \
    -machine q35,accel=$ACCEL \
    -cpu qemu64,+x2apic,+rdrand,+rdseed \
    -smp cores=4,threads=1 \
    -m 512M \
    -bios "$OVMF" \
    -drive file="$IMAGE",format=raw,if=none,id=disk0 \
    -device nvme,serial=helios0,drive=disk0 \
    -device virtio-gpu-pci \
    -device virtio-keyboard-pci \
    -device virtio-mouse-pci \
    -serial stdio \
    -monitor telnet:127.0.0.1:55555,server,nowait \
    -d guest_errors,unimp \
    -no-reboot \
    $DEBUG_FLAGS
