/*
 * sched_asm.S — Cortex-M0+ PendSV context-switch handler
 *
 * picoOS / RP2040
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
 * either MSP or PSP.  picoOS runs all threads with PSP; the kernel
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
 *   saved_sp   is at byte offset 16 within tcb_t.
 *   exc_return is at byte offset 20 within tcb_t.
 *   See _Static_asserts in task.c.
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
    /* M33/M4/M7 support STMDB directly for high registers (r8-r11),     */
    /* replacing 9 instructions with one.  M0+ lacks this, so we use a   */
    /* staging-register workaround with MOV + STMIA.                      */
    /* ------------------------------------------------------------------ */

#if defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_7M__)
    /* Cortex-M33/M4/M7: STMDB handles high registers directly. */
    stmdb   r0!, {r4-r11}       /* r0 = new saved_sp (decremented by 32) */
#else
    /* Cortex-M0+: no STMDB for r8-r11; use staging-register workaround. */
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
#endif

    /* ------------------------------------------------------------------ */
    /* Step 4 — Store the new PSP into current_tcb->saved_sp, and save   */
    /*          LR (the EXC_RETURN value) into current_tcb->exc_return.  */
    /*                                                                    */
    /* tcb_t layout:                                                      */
    /*   tid(4) pid(4) stack_base(4) stack_size(4) saved_sp  exc_return  */
    /*   0      4      8             12            16         20          */
    /*                                                                    */
    /* On Cortex-M33 (RP2350) the FPU may be active.  If so, the CPU     */
    /* saves an extended 26-word frame and sets LR = 0xFFFFFFED.  Saving */
    /* LR here lets PendSV restore the correct frame size per thread.    */
    /* ------------------------------------------------------------------ */
    ldr     r1, =current_tcb        /* r1 = &current_tcb (the global ptr) */
    ldr     r1, [r1]                /* r1 = current_tcb (the TCB pointer)  */
    str     r0, [r1, #16]           /* current_tcb->saved_sp   = r0        */
    /* STR cannot encode lr (r14) in the Thumb narrow form used by M0+.   */
    /* Copy LR to r0 (low register) then store; r0 is free after the str. */
    mov     r0, lr
    str     r0, [r1, #20]           /* current_tcb->exc_return = LR        */

    /* ------------------------------------------------------------------ */
    /* Step 5 — Call sched_next_thread() to get the next TCB.            */
    /*                                                                    */
    /* We push LR (the EXC_RETURN value) so it survives the BL, then    */
    /* pop to balance the stack.  The actual EXC_RETURN to use on return */
    /* is loaded from the incoming thread's TCB in step 7.               */
    /* ------------------------------------------------------------------ */
    push    {lr}
    bl      sched_next_thread       /* r0 = next TCB pointer              */
    pop     {r1}                    /* balance stack (value discarded)    */

    /* ------------------------------------------------------------------ */
    /* Step 6 — Update current_tcb to point to the new thread.           */
    /* ------------------------------------------------------------------ */
    ldr     r1, =current_tcb
    str     r0, [r1]                /* current_tcb = next TCB             */

    /* ------------------------------------------------------------------ */
    /* Step 7 — Load the new thread's EXC_RETURN and saved SP.           */
    /*                                                                    */
    /* Load exc_return first (while r0 = next_tcb), then overwrite r0    */
    /* with saved_sp for the register-restore step.                       */
    /* ------------------------------------------------------------------ */
#if defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_7M__)
    ldr     lr, [r0, #20]           /* LR = next_tcb->exc_return          */
    ldr     r0, [r0, #16]           /* r0 = next_tcb->saved_sp            */
#else
    /* M0+ has no FPU; exc_return is always 0xFFFFFFFD.  Restore LR the  */
    /* old way so bx lr at step 11 still works.                           */
    ldr     r1, [r0, #20]           /* r1 = next_tcb->exc_return (=0xFFFFFFFD) */
    mov     lr, r1                  /* LR  = exc_return                   */
    ldr     r0, [r0, #16]           /* r0  = next_tcb->saved_sp           */
#endif

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

#if defined(__ARM_ARCH_8M_MAIN__) || defined(__ARM_ARCH_7M__)
    /* Cortex-M33/M4/M7: LDMIA handles high registers directly. */
    ldmia   r0!, {r4-r11}       /* r0 = saved_sp + 32 = hw frame base   */
#else
    /* Cortex-M0+: staging-register workaround. */
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
#endif

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
    /* Step 11 — Exception return using the incoming thread's EXC_RETURN. */
    /*                                                                    */
    /* LR was loaded from next_tcb->exc_return in step 7.                */
    /*   0xFFFFFFFD — basic frame (8 words), thread mode, PSP            */
    /*   0xFFFFFFED — extended frame (26 words, includes FP regs), M33   */
    /* The CPU pops the appropriate frame from the PSP stack.             */
    /* ------------------------------------------------------------------ */
    bx      lr

.size isr_pendsv, . - isr_pendsv
