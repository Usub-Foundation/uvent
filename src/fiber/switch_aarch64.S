/*
 * uvent fiber context switch, AArch64 AAPCS64 (Linux, *BSD, macOS).
 *
 *   void uvent_fctx_switch(void** from_sp, void* to_sp);      // x0, x1
 *   void uvent_fctx_trampoline(void);                          // never called directly
 *
 * Callee-saved state stored on the current stack (176 bytes):
 *   x19-x28, x29 (fp), x30 (lr), d8-d15, fpcr.
 * A fresh context is prepared by C++ (Context::create) so that the first switch
 * "returns" into uvent_fctx_trampoline with x19 = entry, x20 = arg.
 *
 * Saved frame layout (offsets from the saved sp):
 *   +0   x19 x20   +16  x21 x22   +32  x23 x24   +48  x25 x26   +64  x27 x28
 *   +80  x29 x30   +96  d8  d9    +112 d10 d11   +128 d12 d13   +144 d14 d15
 *   +160 fpcr      +168 (padding, keeps the frame 16-byte aligned)
 * Only the low 64 bits of v8-v15 are callee-saved by the ABI, hence d8-d15.
 * x0-x18 and v0-v7/v16-v31 are caller-saved: dead across a call, not stored.
 */

#if defined(__aarch64__)                         /* whole file is a no-op on other targets */

#if defined(__APPLE__)
#define SYM(x) _##x                              /* Mach-O prefixes C symbols with '_' */
#define TYPE_FUNC(x)                             /* no .type / .size directives in Mach-O */
#define SIZE_FUNC(x)
#else
#define SYM(x) x                                 /* ELF: symbol name is used as-is */
#define TYPE_FUNC(x) .type x, %function          /* mark as a function for the linker/debugger */
#define SIZE_FUNC(x) .size x, .-x                /* record the function size (. = here) */
#endif

#define FRAME 176                                /* saved-state frame size, multiple of 16 */

    .text                                        /* code section */

    .globl SYM(uvent_fctx_switch)                /* export the symbol */
    TYPE_FUNC(SYM(uvent_fctx_switch))
    .p2align 4                                   /* align entry to 16 bytes */
SYM(uvent_fctx_switch):
    .cfi_startproc                               /* begin unwind info for this function */
    hint #34                        /* bti c */  /* valid indirect-call target for BTI-enabled
                                                    binaries; a NOP on CPUs without BTI */

    /* --- save the callee-saved state of the current context on its own stack --- */
    sub  sp, sp, #FRAME                          /* reserve the frame (sp must stay 16-aligned) */
    .cfi_adjust_cfa_offset FRAME                 /* CFA (canonical frame address) moved by FRAME */
    stp  x19, x20, [sp, #0]                      /* x19 (entry on first switch), x20 (arg) */
    stp  x21, x22, [sp, #16]                     /* callee-saved x21, x22 */
    stp  x23, x24, [sp, #32]                     /* callee-saved x23, x24 */
    stp  x25, x26, [sp, #48]                     /* callee-saved x25, x26 */
    stp  x27, x28, [sp, #64]                     /* callee-saved x27, x28 */
    stp  x29, x30, [sp, #80]                     /* frame pointer, link register (return addr) */
    .cfi_rel_offset x29, 80                      /* x29 is saved at CFA-relative offset 80 */
    .cfi_rel_offset x30, 88                      /* x30 is saved at CFA-relative offset 88 */
    stp  d8,  d9,  [sp, #96]                     /* callee-saved FP/SIMD low halves d8, d9 */
    stp  d10, d11, [sp, #112]                    /* d10, d11 */
    stp  d12, d13, [sp, #128]                    /* d12, d13 */
    stp  d14, d15, [sp, #144]                    /* d14, d15 */
    mrs  x2, fpcr                                /* read FP control register (rounding, traps) */
    str  x2, [sp, #160]                          /* store it in the frame */

    /* --- swap stacks: publish our sp, adopt the target's sp --- */
    mov  x2, sp                                  /* sp cannot be stored directly: copy to x2 */
    str  x2, [x0]                                /* *from_sp = sp (where to resume us later) */
    mov  sp, x1                                  /* sp = to_sp (target's saved frame) */

    /* --- restore the target's callee-saved state from its stack --- */
    ldr  x2, [sp, #160]                          /* load the target's fpcr value */
    msr  fpcr, x2                                /* write FP control register */
    ldp  d14, d15, [sp, #144]                    /* restore d14, d15 */
    ldp  d12, d13, [sp, #128]                    /* restore d12, d13 */
    ldp  d10, d11, [sp, #112]                    /* restore d10, d11 */
    ldp  d8,  d9,  [sp, #96]                     /* restore d8, d9 */
    ldp  x29, x30, [sp, #80]                     /* restore frame pointer and return address */
    ldp  x27, x28, [sp, #64]                     /* restore x27, x28 */
    ldp  x25, x26, [sp, #48]                     /* restore x25, x26 */
    ldp  x23, x24, [sp, #32]                     /* restore x23, x24 */
    ldp  x21, x22, [sp, #16]                     /* restore x21, x22 */
    ldp  x19, x20, [sp, #0]                      /* restore x19 (entry), x20 (arg) */
    add  sp, sp, #FRAME                          /* release the frame */
    .cfi_adjust_cfa_offset -FRAME
    ret                                          /* jump to x30: continue the target (its own
                                                    caller of uvent_fctx_switch, or
                                                    uvent_fctx_trampoline for a fresh context) */
    .cfi_endproc                                 /* end unwind info */
    SIZE_FUNC(SYM(uvent_fctx_switch))

    .globl SYM(uvent_fctx_trampoline)
    TYPE_FUNC(SYM(uvent_fctx_trampoline))
    .p2align 4
SYM(uvent_fctx_trampoline):
    .cfi_startproc
    /* Terminate unwinding here: there is no caller frame on a fiber stack. */
    .cfi_undefined x30                           /* tells unwinders/debuggers the backtrace ends */
    hint #34                        /* bti c */  /* BTI landing pad: we arrive here via 'ret' */
    mov  x0, x20                                 /* first argument = arg (from x20) */
    blr  x19                                     /* call entry(arg); sets x30 to the next insn */
    brk  #0                                      /* entry must never return: trap if it does */
    .cfi_endproc
    SIZE_FUNC(SYM(uvent_fctx_trampoline))

#endif /* __aarch64__ */

#if defined(__ELF__)
    .section .note.GNU-stack, "", %progbits      /* ask the linker for a non-executable stack */
#endif
