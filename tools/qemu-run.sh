#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════════
# Helios OS — QEMU Launch Script
# ═══════════════════════════════════════════════════════════════════════════════
# Usage:
#   ./qemu-run.sh <image>            Run normally
#   ./qemu-run.sh <image> --debug    Run with GDB stub on port 1234
#
# WHY -drive if=pflash AND NOT -bios
# ────────────────────────────────────
# OVMF is a UEFI firmware image, not a legacy PC BIOS ROM.
# The -bios flag loads SeaBIOS (legacy BIOS) and cannot parse OVMF.
# UEFI firmware is stored on two flash chips:
#   pflash unit 0 — OVMF_CODE  (firmware code, read-only)
#   pflash unit 1 — OVMF_VARS  (NVRAM / EFI variables, writable in production)
# Both must be presented as pflash drives so q35's firmware-flash controller
# finds them at the correct memory-mapped addresses.
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
    DEBUG_FLAGS="-s -S"
    echo "  GDB mode: waiting for connection on localhost:1234"
    echo "  Connect with: gdb build/kernel/kernel.elf -ex 'target remote :1234'"
fi

# ── Locate OVMF_CODE (firmware code — read-only pflash unit 0) ────────────
OVMF_CODE=""
for candidate in \
    /usr/share/edk2/x64/OVMF_CODE.4m.fd \
    /usr/share/edk2/x64/OVMF_CODE.fd \
    /usr/share/OVMF/OVMF_CODE.fd \
    /usr/share/ovmf/OVMF_CODE.fd \
    /usr/share/edk2-ovmf/x64/OVMF_CODE.fd; do
    if [[ -f "$candidate" ]]; then
        OVMF_CODE="$candidate"
        break
    fi
done

if [[ -z "$OVMF_CODE" ]]; then
    echo "Error: OVMF_CODE firmware not found."
    echo "Install with: sudo pacman -S edk2-ovmf   (Arch)"
    echo "              sudo apt install ovmf        (Debian/Ubuntu)"
    exit 1
fi

# ── Locate OVMF_VARS (EFI NVRAM — pflash unit 1) ─────────────────────────
# For Phase 0 development we open it read-only; we do not need persistent
# EFI variable storage yet.  If you need to save boot entries across runs,
# copy this file to a local path and remove readonly=on.
OVMF_VARS=""
for candidate in \
    /usr/share/edk2/x64/OVMF_VARS.4m.fd \
    /usr/share/edk2/x64/OVMF_VARS.fd \
    /usr/share/OVMF/OVMF_VARS.fd \
    /usr/share/ovmf/OVMF_VARS.fd \
    /usr/share/edk2-ovmf/x64/OVMF_VARS.fd; do
    if [[ -f "$candidate" ]]; then
        OVMF_VARS="$candidate"
        break
    fi
done

echo "  OVMF_CODE : $OVMF_CODE"
if [[ -n "$OVMF_VARS" ]]; then
    echo "  OVMF_VARS : $OVMF_VARS"
else
    echo "  OVMF_VARS : not found (single-file OVMF — may still work)"
fi
echo "  Image     : $IMAGE"
echo ""

# ── Build pflash arguments ─────────────────────────────────────────────────
# unit=0 → CODE chip (read-only, contains UEFI DXE + BDS phases)
# unit=1 → VARS chip (NVRAM; read-only here since we have no persistent state)
PFLASH_ARGS="-drive if=pflash,format=raw,readonly=on,unit=0,file=$OVMF_CODE"
if [[ -n "$OVMF_VARS" ]]; then
    PFLASH_ARGS="$PFLASH_ARGS -drive if=pflash,format=raw,readonly=on,unit=1,file=$OVMF_VARS"
fi

# ── KVM acceleration if available ─────────────────────────────────────────
ACCEL="tcg"
if [[ -e /dev/kvm ]]; then
    ACCEL="kvm"
    echo "  KVM acceleration enabled"
else
    echo "  KVM not available — using TCG (slower)"
fi
echo ""

# ── Launch QEMU ───────────────────────────────────────────────────────────
exec qemu-system-x86_64 \
    -machine q35,accel=$ACCEL \
    -cpu qemu64,+x2apic,+rdrand,+rdseed \
    -smp cores=4,threads=1 \
    -m 512M \
    $PFLASH_ARGS \
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
