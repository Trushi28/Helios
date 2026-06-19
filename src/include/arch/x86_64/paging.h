/**
 * @file paging.h
 * @brief x86-64 page table structures and constants.
 */

#ifndef ARCH_X86_64_PAGING_H
#define ARCH_X86_64_PAGING_H

#include <helios/types.h>

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Page table entry flags                                                    */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define PTE_PRESENT         (1ULL <<  0)
#define PTE_WRITABLE        (1ULL <<  1)
#define PTE_USER            (1ULL <<  2)
#define PTE_WRITE_THROUGH   (1ULL <<  3)
#define PTE_CACHE_DISABLE   (1ULL <<  4)
#define PTE_ACCESSED        (1ULL <<  5)
#define PTE_DIRTY           (1ULL <<  6)
#define PTE_HUGE_PAGE       (1ULL <<  7)   /* PS bit: 2 MiB page (PDE) or 1 GiB page (PDPE) */
#define PTE_GLOBAL          (1ULL <<  8)
#define PTE_NO_EXECUTE      (1ULL << 63)

/* Physical address mask (bits 12–51) */
#define PTE_ADDR_MASK       0x000FFFFFFFFFF000ULL

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Page sizes                                                                */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define PAGE_SIZE_4K        (4096ULL)
#define PAGE_SIZE_2M        (2ULL * 1024 * 1024)
#define PAGE_SIZE_1G        (1ULL * 1024 * 1024 * 1024)

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Page table indices                                                        */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define PML4_INDEX(va)      (((va) >> 39) & 0x1FF)
#define PDPT_INDEX(va)      (((va) >> 30) & 0x1FF)
#define PD_INDEX(va)        (((va) >> 21) & 0x1FF)
#define PT_INDEX(va)        (((va) >> 12) & 0x1FF)

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  CR4 bits                                                                  */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define CR4_PSE             (1ULL <<  4)  /* Page Size Extensions           */
#define CR4_PAE             (1ULL <<  5)  /* Physical Address Extension     */
#define CR4_PGE             (1ULL <<  7)  /* Page Global Enable             */
#define CR4_OSFXSR          (1ULL <<  9)  /* FXSAVE/FXRSTOR support        */
#define CR4_OSXMMEXCPT      (1ULL << 10)  /* Unmasked SIMD FP Exceptions   */
#define CR4_UMIP            (1ULL << 11)  /* User Mode Instruction Prev    */
#define CR4_LA57            (1ULL << 12)  /* 5-level paging                 */
#define CR4_FSGSBASE        (1ULL << 16)  /* RDFSBASE/WRFSBASE             */
#define CR4_PCIDE           (1ULL << 17)  /* PCID Enable                   */
#define CR4_OSXSAVE         (1ULL << 18)  /* XSAVE/XRSTOR enable          */
#define CR4_SMEP            (1ULL << 20)  /* Supervisor Mode Exec Prev     */
#define CR4_SMAP            (1ULL << 21)  /* Supervisor Mode Access Prev   */

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Page Fault error code bits                                                */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define PF_PRESENT          (1ULL << 0)   /* Fault caused by page protection */
#define PF_WRITE            (1ULL << 1)   /* Fault caused by a write         */
#define PF_USER             (1ULL << 2)   /* Fault from user mode            */
#define PF_RESERVED         (1ULL << 3)   /* Reserved bit violation          */
#define PF_INSN_FETCH       (1ULL << 4)   /* Fault on instruction fetch      */

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  SASOS virtual address layout constants                                    */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define KERNEL_VMA              0xFFFFFFFFFF000000ULL
#define KERNEL_PHYS_MAP_BASE    0xFFFF800000000000ULL
#define KERNEL_HEAP_BASE        0xFFFFFFFFC0000000ULL
#define DEVICE_MMIO_BASE        0xFFFFFFFE00000000ULL

#endif /* ARCH_X86_64_PAGING_H */
