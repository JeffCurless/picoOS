/*
 * sched_asm.S — Cortex-M0+ PendSV context-switch handler
 *
 * PicoTeachOS / RP2040
 *
 * This file contains the only hand-written assembly in the kernel.  It is
 * deliberately kept as short and readable as possible so students can trace
 * through every step.
 *
 * Background: ARM Cortex-M exception handling
 * -------------------------------------------
 * When the CPU takes an exception (e.g. PendSV) it automatically saves
 * {r0, r1, r2, r3, r12, lr, pc, xpsr} onto the *current* stack before
 * branching to the ISR.  On M0+ in Thread mode the stack pointer may be
 * either MSP or PSP.  PicoTeachOS runs all threads with PSP; the kernel
 * exception handlers use MSP.
 *
 * PendSV is configured to the lowest priority (0xFF) so it fires only after
 * every higher-priority ISR has completed.  This means:
 *   - The hardware-saved frame is on the thread's PSP stack.
 *   - The ISR body itself uses MSP (handled automatically by the CPU).
 *
 * Context-switch protocol
 * -----------------------
 * M0+ does NOT have a PUSH/POP for r8-r11, so we use MOV to copy them into
 * the low registers first, then use STMIA/LDMIA.
 *
 * The full saved-context frame on the thread stack (low address first):
 *
 *   ┌─────────┐  ← saved_sp (what we store in tcb->saved_sp)
 *   │  r4     │
 *   │  r5     │
 *   │  r6     │
 *   │  r7     │
 *   │  r8     │
 *   │  r9     │
 *   │  r10    │
 *   │  r11    │
 *   ├─────────┤  ← hardware exception frame saved by CPU
 *   │  r0     │
 *   │  r1     │
 *   │  r2     │
 *   │  r3     │
 *   │  r12    │
 *   │  lr     │
 *   │  pc     │
 *   │  xpsr   │
 *   └─────────┘  ← PSP before exception (= original thread SP)
 *
 * Struct offsets (must match tcb_t in task.h):
 *   saved_sp is at byte offset 16 within tcb_t.
 *   See _Static_assert in task.c.
 */

/*
 * Pull in the Pico SDK's exception-rename macros so that our symbol names
 * match what crt0.S's vector table expects.  When LIB_CMSIS_CORE is defined
 * (which it is whenever cmsis_core is linked), rename_exceptions.h maps the
 * Pico SDK names (isr_pendsv, isr_systick, ...) to the CMSIS names
 * (PendSV_Handler, SysTick_Handler, ...).  crt0.S is compiled with the same
 * defines, so the vector-table slot also uses the CMSIS name.  Without this
 * include the assembler emits "isr_pendsv" which the linker drops as unused.
 */
/* Use the SDK's generated config to get the exception-name macros.
 * We include rename_exceptions.h directly (bypassing boards/pico.h which
 * contains CMake-only directives the assembler cannot parse).
 * LIB_CMSIS_CORE is defined by the build system when cmsis_core is linked. */
#if LIB_CMSIS_CORE
#define PICO_CMSIS_RENAME_EXCEPTIONS 1
#define isr_nmi        NMI_Handler
#define isr_hardfault  HardFault_Handler
#define isr_svcall     SVC_Handler
#define isr_pendsv     PendSV_Handler
#define isr_systick    SysTick_Handler
#endif

.syntax unified
.thumb

.extern sched_next_thread
.extern current_tcb

.global isr_pendsv
.type   isr_pendsv, %function

isr_pendsv:
    /* ------------------------------------------------------------------ */
    /* Step 1 — Disable interrupts to make the switch atomic.             */
    /* ------------------------------------------------------------------ */
    cpsid   i

    /* ------------------------------------------------------------------ */
    /* Step 2 — Read the current thread's PSP.                            */
    /*                                                                    */
    /* The CPU already saved {r0-r3, r12, lr, pc, xpsr} onto the PSP     */
    /* stack when PendSV fired.  We now need to save the remaining        */
    /* callee-saved registers (r4-r11) manually.                          */
    /* ------------------------------------------------------------------ */
    mrs     r0, psp

    /* ------------------------------------------------------------------ */
    /* Step 3 — Push r4-r11 below the hardware frame.                     */
    /*                                                                    */
    /* M0+ only has STMIA/LDMIA for the low registers (r0-r7), so we     */
    /* must copy r8-r11 into r4-r7 first, save them, then restore r4-r7. */
    /* ------------------------------------------------------------------ */

    /* Make room for 8 words (32 bytes) on the PSP stack. */
    subs    r0, r0, #32

    /* Save r4-r7 at the lower four words. */
    stmia   r0!, {r4-r7}

    /* Copy r8-r11 into r4-r7 so we can store them with STMIA. */
    mov     r4, r8
    mov     r5, r9
    mov     r6, r10
    mov     r7, r11

    /* Save the copies (r8-r11 values) at the upper four words. */
    stmia   r0!, {r4-r7}

    /* r0 now points past all 8 saved words.  Back up to the start so  */
    /* r0 = saved_sp = address of the r4 word (bottom of our frame).   */
    subs    r0, r0, #32

    /* ------------------------------------------------------------------ */
    /* Step 4 — Store the new PSP into current_tcb->saved_sp.            */
    /*                                                                    */
    /* tcb_t layout: tid(4) pid(4) stack_base(4) stack_size(4) saved_sp  */
    /* byte offset:   0      4      8             12            16        */
    /* ------------------------------------------------------------------ */
    ldr     r1, =current_tcb        /* r1 = &current_tcb (the global ptr) */
    ldr     r1, [r1]                /* r1 = current_tcb (the TCB pointer)  */
    str     r0, [r1, #16]           /* current_tcb->saved_sp = r0          */

    /* ------------------------------------------------------------------ */
    /* Step 5 — Call sched_next_thread() to get the next TCB.            */
    /*                                                                    */
    /* We push LR (the EXC_RETURN value) so it survives the BL.          */
    /* Note: on M0+ we must use the MSP for ISR work (it was switched    */
    /* automatically when PendSV fired), so this push/pop uses MSP.      */
    /* ------------------------------------------------------------------ */
    push    {lr}
    bl      sched_next_thread       /* r0 = next TCB pointer              */
    pop     {r1}                    /* r1 = saved EXC_RETURN              */
    mov     lr, r1

    /* ------------------------------------------------------------------ */
    /* Step 6 — Update current_tcb to point to the new thread.           */
    /* ------------------------------------------------------------------ */
    ldr     r1, =current_tcb
    str     r0, [r1]                /* current_tcb = next TCB             */

    /* ------------------------------------------------------------------ */
    /* Step 7 — Load the new thread's saved SP.                          */
    /* ------------------------------------------------------------------ */
    ldr     r0, [r0, #16]           /* r0 = next_tcb->saved_sp            */

    /* ------------------------------------------------------------------ */
    /* Step 8 — Restore r4-r11 from the new thread's stack.              */
    /*                                                                    */
    /* The saved frame at r0 (= saved_sp) is:                            */
    /*   [r0+ 0] = saved r4                                              */
    /*   [r0+ 4] = saved r5                                              */
    /*   [r0+ 8] = saved r6                                              */
    /*   [r0+12] = saved r7                                              */
    /*   [r0+16] = saved r8                                              */
    /*   [r0+20] = saved r9                                              */
    /*   [r0+24] = saved r10                                             */
    /*   [r0+28] = saved r11                                             */
    /* Hardware frame follows at [r0+32].                                */
    /*                                                                    */
    /* M0+ cannot LDMIA into r8-r11 directly, so we use r4-r7 as a      */
    /* staging area.  We must restore r4-r7 last to avoid clobbering.   */
    /* ------------------------------------------------------------------ */

    /* Load the r8-r11 values (at offset +16) into r4-r7 as staging. */
    adds    r0, r0, #16             /* r0 = &saved_r8                    */
    ldmia   r0!, {r4-r7}            /* r4=r8val,r5=r9val,r6=r10val,r7=r11val */
                                    /* r0 now = saved_sp + 32 = hw frame base */

    /* Move staged values to their real destinations. */
    mov     r8,  r4
    mov     r9,  r5
    mov     r10, r6
    mov     r11, r7

    /* Back up to load the real r4-r7 values (at saved_sp + 0). */
    subs    r0, r0, #32             /* r0 = saved_sp again               */
    ldmia   r0!, {r4-r7}            /* restore r4-r7; r0 = saved_sp + 16 */

    /* Advance r0 to the hardware exception frame base (saved_sp + 32). */
    adds    r0, r0, #16

    /* ------------------------------------------------------------------ */
    /* Step 9 — Update PSP to the new thread's stack pointer.            */
    /* r0 now points to the bottom of the hardware exception frame.       */
    /* The CPU will pop {r0-r3, r12, lr, pc, xpsr} from here on return. */
    /* ------------------------------------------------------------------ */
    msr     psp, r0

    /* ------------------------------------------------------------------ */
    /* Step 10 — Re-enable interrupts.                                    */
    /* ------------------------------------------------------------------ */
    cpsie   i

    /* ------------------------------------------------------------------ */
    /* Step 11 — Exception return.                                        */
    /*                                                                    */
    /* EXC_RETURN = 0xFFFFFFFD means:                                     */
    /*   - Return to Thread mode                                          */
    /*   - Use PSP for the frame                                          */
    /*   - Restore floating-point context: no (M0+ has none)             */
    /* The CPU pops {r0-r3, r12, lr, pc, xpsr} from the PSP stack.       */
    /* ------------------------------------------------------------------ */
    ldr     r0, =0xFFFFFFFD
    bx      r0

.size isr_pendsv, . - isr_pendsv
