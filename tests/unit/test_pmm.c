/**
 * @file test_pmm.c
 * @brief Host-side unit tests for the PMM buddy allocator.
 *
 * Uses a static array as fake physical memory. Builds a mock boot_info_t
 * and memory map, then exercises the PMM API and verifies correctness.
 *
 * Compile and run on the HOST with system gcc:
 *   gcc -DUNIT_TEST -I../../src/include -std=c2x -o test_pmm \
 *       test_pmm.c ../../src/kernel/mm/pmm.c ../../src/kernel/string.c
 */

/* UNIT_TEST is defined via -DUNIT_TEST in the Makefile */

/* Kernel headers first — types.h detects UNIT_TEST and includes
 * system stdint.h/stdbool.h/stddef.h instead of manual typedefs */
#include <helios/types.h>
#include <helios/boot_info.h>
#include <helios/pmm.h>

/* System headers for test harness I/O */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Fake physical memory                                                      */
/* ═══════════════════════════════════════════════════════════════════════════ */

/* 16 MiB of fake physical memory, starting at a "physical address" of 1 MiB
 * (to avoid the reserved low 1 MiB region). */
#define FAKE_MEM_SIZE   (16ULL * 1024 * 1024)
#define FAKE_MEM_BASE   (1ULL * 1024 * 1024)   /* 1 MiB */

static uint8_t g_fake_mem[FAKE_MEM_SIZE] __attribute__((aligned(4096)));

/* The PMM's phys_to_virt() in UNIT_TEST mode uses this global */
uint8_t *g_fake_phys_base;

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Test infrastructure                                                       */
/* ═══════════════════════════════════════════════════════════════════════════ */

static int g_tests_run    = 0;
static int g_tests_passed = 0;

#define TEST(name) \
    do { \
        g_tests_run++; \
        printf("  [TEST] %-50s ", name); \
    } while (0)

#define PASS() \
    do { \
        g_tests_passed++; \
        printf("PASS\n"); \
    } while (0)

#define FAIL(msg) \
    do { \
        printf("FAIL: %s\n", msg); \
    } while (0)

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { FAIL(msg); return; } \
    } while (0)

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Setup helper — reinitializes the PMM with a fresh fake memory map         */
/* ═══════════════════════════════════════════════════════════════════════════ */

static helios_mem_entry_t g_mem_entries[4];
static boot_info_t        g_boot_info;

static void setup_pmm(void) {
    /* Reset fake memory */
    memset(g_fake_mem, 0, FAKE_MEM_SIZE);

    /* Set up the pointer that pmm.c uses in UNIT_TEST mode:
     * Physical address N maps to g_fake_mem[N - FAKE_MEM_BASE]
     * But for simplicity, we set g_fake_phys_base so that
     * phys_to_virt(phys) = g_fake_phys_base + phys
     * We need g_fake_phys_base + FAKE_MEM_BASE = &g_fake_mem[0]
     * So g_fake_phys_base = &g_fake_mem[0] - FAKE_MEM_BASE */
    g_fake_phys_base = g_fake_mem - FAKE_MEM_BASE;

    /* Build a fake memory map:
     * [0] 0x0 - 0xFFFFF: RESERVED (low 1 MiB — PMM skips this)
     * [1] FAKE_MEM_BASE - FAKE_MEM_BASE + FAKE_MEM_SIZE: FREE
     * [2] Kernel region: small BOOTLOADER region (avoided by PMM)
     */
    g_mem_entries[0].phys_base  = 0;
    g_mem_entries[0].page_count = 0x100000 / PAGE_SIZE;  /* 256 pages = 1 MiB */
    g_mem_entries[0].type       = HELIOS_MEM_RESERVED;
    g_mem_entries[0].attributes = 0;

    g_mem_entries[1].phys_base  = FAKE_MEM_BASE;
    g_mem_entries[1].page_count = FAKE_MEM_SIZE / PAGE_SIZE;
    g_mem_entries[1].type       = HELIOS_MEM_FREE;
    g_mem_entries[1].attributes = 0;

    /* boot_info */
    memset(&g_boot_info, 0, sizeof(g_boot_info));
    g_boot_info.magic   = BOOT_INFO_MAGIC;
    g_boot_info.version = BOOT_INFO_VERSION;

    g_boot_info.memory_map.entries_phys = (uint64_t)(uintptr_t)g_mem_entries;
    g_boot_info.memory_map.entry_count  = 2;
    g_boot_info.memory_map.entry_size   = sizeof(helios_mem_entry_t);

    /* Kernel "image" at a location that does not overlap the free region
     * — put it at physical 0x80000 (512 KiB) within the reserved low 1 MiB */
    g_boot_info.kernel.phys_base    = 0x80000;
    g_boot_info.kernel.size         = 0x10000;  /* 64 KiB */
    g_boot_info.kernel.entry_offset = 0;

    /* Initialize PMM */
    pmm_init(&g_boot_info);
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Test cases                                                                */
/* ═══════════════════════════════════════════════════════════════════════════ */

static void test_alloc_order0(void) {
    TEST("alloc order 0 returns non-zero");
    setup_pmm();
    phys_addr_t addr = pmm_alloc_pages(0);
    ASSERT(addr != 0, "expected non-zero address");
    ASSERT((addr % PAGE_SIZE) == 0, "expected page-aligned");
    PASS();
}

static void test_alloc_order0_twice_different(void) {
    TEST("alloc order 0 twice returns different addresses");
    setup_pmm();
    phys_addr_t a1 = pmm_alloc_pages(0);
    phys_addr_t a2 = pmm_alloc_pages(0);
    ASSERT(a1 != 0, "first alloc failed");
    ASSERT(a2 != 0, "second alloc failed");
    ASSERT(a1 != a2, "expected different addresses");
    PASS();
}

static void test_alloc_free_realloc(void) {
    TEST("alloc-free-realloc order 0 returns same address");
    setup_pmm();
    phys_addr_t a1 = pmm_alloc_pages(0);
    ASSERT(a1 != 0, "first alloc failed");
    pmm_free_pages(a1, 0);
    phys_addr_t a2 = pmm_alloc_pages(0);
    ASSERT(a2 == a1, "expected same address after free+realloc");
    PASS();
}

static void test_alloc_order1_aligned(void) {
    TEST("alloc order 1 is 8 KiB aligned");
    setup_pmm();
    phys_addr_t addr = pmm_alloc_pages(1);
    ASSERT(addr != 0, "alloc failed");
    ASSERT((addr % (8 * 1024)) == 0, "expected 8 KiB alignment");
    PASS();
}

static void test_buddy_coalesce(void) {
    TEST("buddy coalescing: free two order-0 buddies");
    setup_pmm();
    uint64_t initial_free = pmm_free_page_count();

    /* Allocate an order-1 block (which gives us two adjacent order-0 pages) */
    phys_addr_t block = pmm_alloc_pages(1);
    ASSERT(block != 0, "alloc order 1 failed");
    uint64_t after_alloc = pmm_free_page_count();
    ASSERT(after_alloc == initial_free - 2, "expected 2 pages consumed");

    /* Free as two separate order-0 pages */
    phys_addr_t buddy0 = block;
    phys_addr_t buddy1 = block + PAGE_SIZE;
    pmm_free_pages(buddy0, 0);
    pmm_free_pages(buddy1, 0);

    uint64_t after_free = pmm_free_page_count();
    ASSERT(after_free == initial_free, "expected full coalesce back to initial");
    PASS();
}

static void test_oom_graceful(void) {
    TEST("OOM returns 0, no crash");
    setup_pmm();

    /* Allocate until OOM */
    uint64_t alloc_count = 0;
    while (1) {
        phys_addr_t addr = pmm_alloc_pages(0);
        if (addr == 0) break;
        alloc_count++;
        /* Safety: don't loop forever */
        if (alloc_count > FAKE_MEM_SIZE / PAGE_SIZE + 100) {
            FAIL("allocated more pages than exist");
            return;
        }
    }
    ASSERT(alloc_count > 0, "should have allocated at least one page");
    /* Verify another alloc still returns 0 */
    phys_addr_t extra = pmm_alloc_pages(0);
    ASSERT(extra == 0, "OOM should return 0");
    PASS();
}

static void test_free_all_restores_count(void) {
    TEST("free all blocks restores initial free count");
    setup_pmm();
    uint64_t initial_free = pmm_free_page_count();

    /* Allocate a bunch of pages and track them */
    #define MAX_TRACK 4096
    static phys_addr_t addrs[MAX_TRACK];
    uint64_t count = 0;

    while (count < MAX_TRACK) {
        phys_addr_t addr = pmm_alloc_pages(0);
        if (addr == 0) break;
        addrs[count++] = addr;
    }
    ASSERT(count > 0, "should have allocated at least one page");
    ASSERT(pmm_free_page_count() < initial_free, "free count should decrease");

    /* Free all */
    for (uint64_t i = 0; i < count; i++) {
        pmm_free_pages(addrs[i], 0);
    }

    uint64_t final_free = pmm_free_page_count();
    ASSERT(final_free == initial_free,
           "free count should equal initial after freeing all");
    PASS();
    #undef MAX_TRACK
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Main                                                                      */
/* ═══════════════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("\n");
    printf("  ═══════════════════════════════════════════════════\n");
    printf("  Helios PMM Unit Tests\n");
    printf("  ═══════════════════════════════════════════════════\n\n");

    test_alloc_order0();
    test_alloc_order0_twice_different();
    test_alloc_free_realloc();
    test_alloc_order1_aligned();
    test_buddy_coalesce();
    test_oom_graceful();
    test_free_all_restores_count();

    printf("\n  Results: %d/%d tests passed\n\n", g_tests_passed, g_tests_run);

    if (g_tests_passed != g_tests_run) {
        return 1;
    }
    return 0;
}
