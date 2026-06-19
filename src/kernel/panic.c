/**
 * @file panic.c
 * @brief Kernel panic handler — last resort error reporting.
 */

#include <helios/types.h>

/* Forward declarations */
extern void serial_puts(const char *s);
extern void serial_printf(const char *fmt, ...);

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Kernel panic                                                              */
/* ═══════════════════════════════════════════════════════════════════════════ */

/**
 * @brief Halt the system with a panic message.
 *
 * Prints the message and register state to serial, then enters an
 * infinite halt loop. Interrupts are disabled and never re-enabled.
 */
NORETURN void panic(const char *msg) {
    cli();

    serial_puts("\n\n");
    serial_puts("╔══════════════════════════════════════════════════════════╗\n");
    serial_puts("║                  HELIOS KERNEL PANIC                    ║\n");
    serial_puts("╚══════════════════════════════════════════════════════════╝\n");
    serial_puts("\n  Reason: ");
    serial_puts(msg);
    serial_puts("\n\n");

    /* Dump key registers */
    uint64_t rsp, rbp, cr0, cr2, cr3, cr4;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    __asm__ volatile("mov %%rbp, %0" : "=r"(rbp));
    cr0 = read_cr0();
    cr2 = read_cr2();
    cr3 = read_cr3();
    cr4 = read_cr4();

    serial_printf("  RSP = %p\n", (void *)rsp);
    serial_printf("  RBP = %p\n", (void *)rbp);
    serial_printf("  CR0 = %p\n", (void *)cr0);
    serial_printf("  CR2 = %p  (last page fault address)\n", (void *)cr2);
    serial_printf("  CR3 = %p  (page table root)\n", (void *)cr3);
    serial_printf("  CR4 = %p\n", (void *)cr4);

    serial_puts("\n  System halted. Reset required.\n");

    /* Infinite halt loop */
    for (;;) {
        __asm__ volatile("hlt");
    }
}

/**
 * @brief Panic with an interrupt frame dump (called from exception handlers).
 */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no;
    uint64_t error_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} interrupt_frame_t;

NORETURN void panic_with_frame(const char *msg, interrupt_frame_t *frame) {
    cli();

    serial_puts("\n\n");
    serial_puts("╔══════════════════════════════════════════════════════════╗\n");
    serial_puts("║                  HELIOS KERNEL PANIC                    ║\n");
    serial_puts("╚══════════════════════════════════════════════════════════╝\n");
    serial_puts("\n  Reason: ");
    serial_puts(msg);
    serial_printf("\n  Vector: 0x%02x  Error code: 0x%016lx\n",
                  (unsigned int)frame->int_no, frame->error_code);

    serial_puts("\n  ── Register State ──\n");
    serial_printf("  RAX=%016lx  RBX=%016lx  RCX=%016lx\n", frame->rax, frame->rbx, frame->rcx);
    serial_printf("  RDX=%016lx  RSI=%016lx  RDI=%016lx\n", frame->rdx, frame->rsi, frame->rdi);
    serial_printf("  R8 =%016lx  R9 =%016lx  R10=%016lx\n", frame->r8,  frame->r9,  frame->r10);
    serial_printf("  R11=%016lx  R12=%016lx  R13=%016lx\n", frame->r11, frame->r12, frame->r13);
    serial_printf("  R14=%016lx  R15=%016lx  RBP=%016lx\n", frame->r14, frame->r15, frame->rbp);
    serial_printf("  RIP=%016lx  CS =%04lx  RFLAGS=%016lx\n", frame->rip, frame->cs, frame->rflags);
    serial_printf("  RSP=%016lx  SS =%04lx\n", frame->rsp, frame->ss);
    serial_printf("  CR2=%016lx  (page fault address)\n", read_cr2());

    serial_puts("\n  System halted. Reset required.\n");

    for (;;) {
        __asm__ volatile("hlt");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════ */
/*  Stack protector support                                                   */
/* ═══════════════════════════════════════════════════════════════════════════ */

uint64_t __stack_chk_guard = 0xDEADBEEFCAFEBABEULL;

NORETURN void __stack_chk_fail(void) {
    panic("Stack smashing detected!");
}
