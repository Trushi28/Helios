/**
 * @file types.h
 * @brief Helios OS — Freestanding base types and utility macros.
 *
 * This header provides integer types, boolean, size types, and compiler
 * intrinsic wrappers for use in both the bootloader and the kernel.
 * No standard library dependency.
 */

#ifndef HELIOS_TYPES_H
#define HELIOS_TYPES_H

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Integer types (freestanding — no <stdint.h> in kernel mode)              */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef unsigned char       uint8_t;
typedef unsigned short      uint16_t;
typedef unsigned int        uint32_t;
typedef unsigned long long  uint64_t;

typedef signed char         int8_t;
typedef signed short        int16_t;
typedef signed int          int32_t;
typedef signed long long    int64_t;

typedef uint64_t            size_t;
typedef int64_t             ssize_t;
typedef uint64_t            uintptr_t;
typedef int64_t             intptr_t;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Boolean                                                                   */
/* ═══════════════════════════════════════════════════════════════════════════ */

/* In C23 (c2x), bool/true/false are keywords — no typedef/macros needed.
 * For pre-C23 compilers, define them manually. */
#if __STDC_VERSION__ < 202311L
typedef _Bool               bool;
#define true                1
#define false               0
#endif

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  NULL                                                                      */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define NULL                ((void *)0)

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Physical / Virtual address types                                          */
/* ═══════════════════════════════════════════════════════════════════════════ */

typedef uint64_t            phys_addr_t;
typedef uint64_t            virt_addr_t;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Compiler attributes                                                       */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define PACKED              __attribute__((packed))
#define ALIGNED(n)          __attribute__((aligned(n)))
#define NORETURN            _Noreturn
#define UNUSED              __attribute__((unused))
#define ALWAYS_INLINE       __attribute__((always_inline)) inline
#define SECTION(s)          __attribute__((section(s)))

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Utility macros                                                            */
/* ═══════════════════════════════════════════════════════════════════════════ */

#define ARRAY_SIZE(a)       (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a)      (((x) + ((a) - 1)) & ~((a) - 1))
#define ALIGN_DOWN(x, a)    ((x) & ~((a) - 1))
#define MIN(a, b)           ((a) < (b) ? (a) : (b))
#define MAX(a, b)           ((a) > (b) ? (a) : (b))

#define PAGE_SIZE           4096ULL
#define PAGE_MASK           (~(PAGE_SIZE - 1))

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Inline I/O port access                                                    */
/* ═══════════════════════════════════════════════════════════════════════════ */

static ALWAYS_INLINE void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static ALWAYS_INLINE uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static ALWAYS_INLINE void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static ALWAYS_INLINE uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static ALWAYS_INLINE void outl(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static ALWAYS_INLINE uint32_t inl(uint16_t port) {
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  CPU control                                                               */
/* ═══════════════════════════════════════════════════════════════════════════ */

static ALWAYS_INLINE void cpu_halt(void) {
    __asm__ volatile("hlt");
}

static ALWAYS_INLINE void cpu_pause(void) {
    __asm__ volatile("pause");
}

static ALWAYS_INLINE void cli(void) {
    __asm__ volatile("cli");
}

static ALWAYS_INLINE void sti(void) {
    __asm__ volatile("sti");
}

static ALWAYS_INLINE uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  CR register access                                                        */
/* ═══════════════════════════════════════════════════════════════════════════ */

static ALWAYS_INLINE uint64_t read_cr0(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr0, %0" : "=r"(val));
    return val;
}

static ALWAYS_INLINE void write_cr0(uint64_t val) {
    __asm__ volatile("mov %0, %%cr0" : : "r"(val));
}

static ALWAYS_INLINE uint64_t read_cr2(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr2, %0" : "=r"(val));
    return val;
}

static ALWAYS_INLINE uint64_t read_cr3(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr3, %0" : "=r"(val));
    return val;
}

static ALWAYS_INLINE void write_cr3(uint64_t val) {
    __asm__ volatile("mov %0, %%cr3" : : "r"(val));
}

static ALWAYS_INLINE uint64_t read_cr4(void) {
    uint64_t val;
    __asm__ volatile("mov %%cr4, %0" : "=r"(val));
    return val;
}

static ALWAYS_INLINE void write_cr4(uint64_t val) {
    __asm__ volatile("mov %0, %%cr4" : : "r"(val));
}

#endif /* HELIOS_TYPES_H */
