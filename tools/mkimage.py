#!/usr/bin/env python3
"""
Helios OS — Disk Image Builder

Creates a GPT disk image with an EFI System Partition (FAT32) containing
the UEFI bootloader and kernel binary.

Usage:
    python3 mkimage.py --bootloader build/boot/BOOTX64.EFI \
                       --kernel build/kernel/kernel.bin \
                       --output build/helios.img
"""

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile


def check_tool(name: str) -> bool:
    """Check if a command-line tool is available."""
    return shutil.which(name) is not None


def run(cmd: list[str], **kwargs):
    """Run a command, exit on failure."""
    print(f"  $ {' '.join(cmd)}")
    result = subprocess.run(cmd, capture_output=True, text=True, **kwargs)
    if result.returncode != 0:
        print(f"ERROR: Command failed: {' '.join(cmd)}")
        print(result.stderr)
        sys.exit(1)
    return result


def create_image(bootloader: str, kernel: str, output: str,
                 image_size_mb: int = 64):
    """Create a GPT disk image with an ESP containing the bootloader and kernel."""

    print(f"\n  Helios Disk Image Builder")
    print(f"  ────────────────────────────────────────")
    print(f"  Bootloader: {bootloader}")
    print(f"  Kernel:     {kernel}")
    print(f"  Output:     {output}")
    print(f"  Image size: {image_size_mb} MiB")
    print()

    # Validate inputs
    for f, label in [(bootloader, "Bootloader"), (kernel, "Kernel")]:
        if not os.path.isfile(f):
            print(f"ERROR: {label} not found: {f}")
            sys.exit(1)

    # Check required tools
    required_tools = ["dd", "mkfs.fat", "mmd", "mcopy"]
    missing = [t for t in required_tools if not check_tool(t)]
    if missing:
        print(f"ERROR: Missing tools: {', '.join(missing)}")
        print("  Install with: sudo apt install dosfstools mtools")
        sys.exit(1)

    os.makedirs(os.path.dirname(output) or ".", exist_ok=True)

    # ── 1. Create empty disk image ──────────────────────────────────────
    print("  [1/4] Creating raw disk image...")
    run(["dd", "if=/dev/zero", f"of={output}",
         "bs=1M", f"count={image_size_mb}", "status=none"])

    # ── 2. Create GPT partition table with a single ESP ─────────────────
    print("  [2/4] Creating GPT + ESP partition...")
    if check_tool("sgdisk"):
        run(["sgdisk",
             "--clear",                          # Create new GPT
             "--new=1:2048:+32M",                # Partition 1: 32 MiB ESP
             "--typecode=1:EF00",                 # EFI System Partition type
             "--change-name=1:EFI System",
             output])
    elif check_tool("parted"):
        run(["parted", "-s", output,
             "mklabel", "gpt",
             "mkpart", "ESP", "fat32", "1MiB", "33MiB",
             "set", "1", "esp", "on"])
    else:
        # Fallback: create a simple image with just the FAT32 filesystem
        # (some OVMF builds can boot from a raw FAT image)
        print("  WARNING: No partitioning tool found (sgdisk/parted).")
        print("           Creating raw FAT32 image (may work with OVMF).")
        _create_raw_fat_image(bootloader, kernel, output, image_size_mb)
        return

    # ── 3. Format the ESP as FAT32 ──────────────────────────────────────
    print("  [3/4] Formatting ESP as FAT32...")

    # Extract the ESP partition region (starting at sector 2048 = 1 MiB)
    esp_file = output + ".esp"
    run(["dd", f"if={output}", f"of={esp_file}",
         "bs=512", "skip=2048", "count=65536", "status=none"])  # 32 MiB

    run(["mkfs.fat", "-F", "32", "-n", "HELIOS", esp_file])

    # ── 4. Copy bootloader and kernel into ESP ──────────────────────────
    print("  [4/4] Installing bootloader and kernel...")
    run(["mmd", "-i", esp_file, "::EFI"])
    run(["mmd", "-i", esp_file, "::EFI/HELIOS"])
    run(["mcopy", "-i", esp_file, bootloader, "::EFI/HELIOS/BOOTX64.EFI"])
    run(["mcopy", "-i", esp_file, kernel, "::EFI/HELIOS/KERNEL.BIN"])

    # Write ESP back into the disk image
    run(["dd", f"if={esp_file}", f"of={output}",
         "bs=512", "seek=2048", "conv=notrunc", "status=none"])

    os.remove(esp_file)

    final_size = os.path.getsize(output)
    print(f"\n  ✓ Image created: {output} ({final_size // 1024} KiB)")
    print()


def _create_raw_fat_image(bootloader: str, kernel: str, output: str,
                           size_mb: int):
    """Fallback: create a raw FAT32 image (no GPT wrapper)."""
    run(["mkfs.fat", "-F", "32", "-n", "HELIOS",
         "-C", output, str(size_mb * 1024)])
    run(["mmd", "-i", output, "::EFI"])
    run(["mmd", "-i", output, "::EFI/HELIOS"])
    run(["mcopy", "-i", output, bootloader, "::EFI/HELIOS/BOOTX64.EFI"])
    run(["mcopy", "-i", output, kernel, "::EFI/HELIOS/KERNEL.BIN"])
    print(f"\n  ✓ Raw FAT32 image: {output}")


def main():
    parser = argparse.ArgumentParser(description="Helios OS Disk Image Builder")
    parser.add_argument("--bootloader", required=True,
                        help="Path to BOOTX64.EFI")
    parser.add_argument("--kernel", required=True,
                        help="Path to kernel.bin")
    parser.add_argument("--output", required=True,
                        help="Output disk image path")
    parser.add_argument("--size", type=int, default=64,
                        help="Disk image size in MiB (default: 64)")

    args = parser.parse_args()
    create_image(args.bootloader, args.kernel, args.output, args.size)


if __name__ == "__main__":
    main()
