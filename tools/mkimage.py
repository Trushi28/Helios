#!/usr/bin/env python3
"""
Helios OS — Disk Image Builder
Creates a GPT disk image with an EFI System Partition (FAT32) containing
the UEFI bootloader and kernel binary.
Usage:
    python3 mkimage.py --bootloader build/boot/BOOTX64.EFI \\
                       --kernel     build/kernel/kernel.bin \\
                       --output     build/helios.img
"""

import argparse
import os
import shutil
import subprocess
import sys


def check_tool(name: str) -> bool:
    """Return True if the named command is available on PATH."""
    return shutil.which(name) is not None


def run(cmd: list[str], **kwargs):
    """Run a command and exit with a clear message on failure."""
    print(f"  $ {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, **kwargs)
    if result.returncode != 0:
        print(f"ERROR: Command failed: {' '.join(cmd)}")
        if result.stderr:
            print(result.stderr)
        sys.exit(1)
    return result


# ── ESP population helper ──────────────────────────────────────────────────

def _populate_esp(image_or_esp: str, bootloader: str, kernel: str) -> None:
    """
    Write the required directory tree into a FAT32 image.

    \EFI\BOOT\BOOTX64.EFI   — UEFI spec fallback discovery path
    \EFI\HELIOS\BOOTX64.EFI — Helios canonical path
    \EFI\HELIOS\KERNEL.BIN  — Flat kernel binary
    """
    i = image_or_esp  # shorthand

    # Create directories
    run(["mmd", "-i", i, "::EFI"])
    run(["mmd", "-i", i, "::EFI/BOOT"])       # UEFI fallback path
    run(["mmd", "-i", i, "::EFI/HELIOS"])     # Helios-specific path

    # UEFI fallback — this is what OVMF finds on a cold boot with no NVRAM
    run(["mcopy", "-i", i, bootloader, "::EFI/BOOT/BOOTX64.EFI"])

    # Helios canonical path — used by future explicit boot manager entries
    run(["mcopy", "-i", i, bootloader, "::EFI/HELIOS/BOOTX64.EFI"])

    # Kernel binary — loaded by bootx64.c from \EFI\HELIOS\KERNEL.BIN
    run(["mcopy", "-i", i, kernel, "::EFI/HELIOS/KERNEL.BIN"])


# ── Main image creation ────────────────────────────────────────────────────

def create_image(bootloader: str, kernel: str, output: str,
                 image_size_mb: int = 64) -> None:
    """Create a GPT disk image with an ESP containing the bootloader and kernel."""

    print()
    print("  Helios Disk Image Builder")
    print("  ─────────────────────────────────────────")
    print(f"  Bootloader : {bootloader}")
    print(f"  Kernel     : {kernel}")
    print(f"  Output     : {output}")
    print(f"  Image size : {image_size_mb} MiB")
    print()

    # Validate inputs
    for path, label in [(bootloader, "Bootloader"), (kernel, "Kernel")]:
        if not os.path.isfile(path):
            print(f"ERROR: {label} not found: {path}")
            sys.exit(1)

    # Check required mtools
    missing = [t for t in ["dd", "mkfs.fat", "mmd", "mcopy"]
               if not check_tool(t)]
    if missing:
        print(f"ERROR: Missing tools: {', '.join(missing)}")
        print("  Install with: sudo apt install dosfstools mtools")
        sys.exit(1)

    os.makedirs(os.path.dirname(output) or ".", exist_ok=True)

    # ── 1. Create raw zeroed disk image ───────────────────────────────
    # Use 128 MiB to comfortably fit a 48 MiB ESP + GPT overhead.
    print("  [1/4] Creating raw disk image...")
    run(["dd", "if=/dev/zero", f"of={output}",
         "bs=1M", f"count={image_size_mb}", "status=none"])

    # ── 2. Partition with GPT + ESP ───────────────────────────────────
    # ESP must be ≥ ~34 MiB so the FAT32 cluster count exceeds the
    # spec-mandated minimum of 65 525.  We use 48 MiB for margin.
    ESP_SIZE_MB = 48
    ESP_SECTORS = ESP_SIZE_MB * 2048       # 98 304 sectors
    print(f"  [2/4] Creating GPT + ESP partition ({ESP_SIZE_MB} MiB)...")
    if check_tool("sgdisk"):
        run(["sgdisk",
             "--clear",
             f"--new=1:2048:+{ESP_SIZE_MB}M",
             "--typecode=1:EF00",
             "--change-name=1:EFI System",
             output])
    elif check_tool("parted"):
        end_mib = 1 + ESP_SIZE_MB          # starts at 1 MiB
        run(["parted", "-s", output,
             "mklabel", "gpt",
             "mkpart", "ESP", "fat32", "1MiB", f"{end_mib}MiB",
             "set", "1", "esp", "on"])
    else:
        print("  WARNING: No partitioning tool (sgdisk/parted) found.")
        print("           Creating raw FAT32 image — may work with OVMF.")
        _create_raw_fat_image(bootloader, kernel, output, image_size_mb)
        return

    # ── 3. Format ESP partition as FAT32 ──────────────────────────────
    print("  [3/4] Formatting ESP as FAT32...")

    # Extract the ESP region starting at sector 2048 (= 1 MiB offset)
    esp_file = output + ".esp.tmp"
    run(["dd", f"if={output}", f"of={esp_file}",
         "bs=512", "skip=2048", f"count={ESP_SECTORS}", "status=none"])

    run(["mkfs.fat", "-F", "32", "-n", "HELIOS", esp_file])

    # ── 4. Install bootloader + kernel into ESP ────────────────────────
    print("  [4/4] Installing bootloader and kernel...")
    _populate_esp(esp_file, bootloader, kernel)

    # Write ESP back into the disk image at sector 2048
    run(["dd", f"if={esp_file}", f"of={output}",
         "bs=512", "seek=2048", "conv=notrunc", "status=none"])

    os.remove(esp_file)

    size = os.path.getsize(output)
    print()
    print(f"  ✓ Image: {output}  ({size // 1024} KiB)")
    print()
    print("  ESP layout:")
    print("    \\EFI\\BOOT\\BOOTX64.EFI   — UEFI fallback discovery path")
    print("    \\EFI\\HELIOS\\BOOTX64.EFI — Helios canonical path")
    print("    \\EFI\\HELIOS\\KERNEL.BIN  — Flat kernel binary")
    print()


def _create_raw_fat_image(bootloader: str, kernel: str, output: str,
                           size_mb: int) -> None:
    """Fallback: raw FAT32 image (no GPT wrapper) for minimal toolchain setups."""
    run(["mkfs.fat", "-F", "32", "-n", "HELIOS",
         "-C", output, str(size_mb * 1024)])
    _populate_esp(output, bootloader, kernel)
    print(f"\n  ✓ Raw FAT32 image: {output}")


# ── CLI ────────────────────────────────────────────────────────────────────

def main() -> None:
    parser = argparse.ArgumentParser(description="Helios OS Disk Image Builder")
    parser.add_argument("--bootloader", required=True,
                        help="Path to BOOTX64.EFI")
    parser.add_argument("--kernel", required=True,
                        help="Path to kernel.bin")
    parser.add_argument("--output", required=True,
                        help="Output disk image path")
    parser.add_argument("--size", type=int, default=128,
                        help="Disk image size in MiB (default: 128)")
    args = parser.parse_args()
    create_image(args.bootloader, args.kernel, args.output, args.size)


if __name__ == "__main__":
    main()
