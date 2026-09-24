/*
 * uvent fiber context switch, x86_64 System V ABI (Linux, *BSD, macOS).
 *
 *   void uvent_fctx_switch(void** from_sp, void* to_sp);      // rdi, rsi
 *   void uvent_fctx_trampoline(void);                          // never called directly
 *
 * Callee-saved state pushed on the current stack: rbp rbx r12-r15 + mxcsr/x87cw.
 * A fresh context is prepared by C++ (Context::create) so that the first switch
 * "returns" into uvent_fctx_trampoline with r12 = entry, r13 = arg.
 *
 * Saved frame layout (grows down, addresses relative to the saved rsp):
 *   +0  mxcsr (4 bytes)   +4  x87 control word (2 bytes, 2 bytes padding)
 *   +8  r15   +16 r14   +24 r13   +32 r12   +40 rbx   +48 rbp   +56 return address
 * Caller-saved registers (rax, rcx, rdx, rsi, rdi, r8-r11, xmm*) are, by the
 * ABI, already dead across a call, so nothing else needs saving.
 */

#if defined(__x86_64__)                          /* whole file is a no-op on other targets */

#if defined(__APPLE__)
#define SYM(x) _##x                              /* Mach-O prefixes C symbols with '_' */
#define TYPE_FUNC(x)                             /* no .type / .size directives in Mach-O */
#define SIZE_FUNC(x)
#else
#define SYM(x) x                                 /* ELF: symbol name is used as-is */
#define TYPE_FUNC(x) .type x, @function          /* mark as a function for the linker/debugger */
#define SIZE_FUNC(x) .size x, .-x                /* record the function size (. = here) */
#endif

    .text                                        /* code section */

    .globl SYM(uvent_fctx_switch)                /* export the symbol */
    TYPE_FUNC(SYM(uvent_fctx_switch))
    .p2align 4                                   /* align entry to 16 bytes */
SYM(uvent_fctx_switch):
    .cfi_startproc                               /* begin unwind info for this function */

    /* --- save the callee-saved state of the current context on its own stack --- */
    pushq %rbp                                   /* frame pointer */
    .cfi_adjust_cfa_offset 8                     /* CFA (canonical frame address) moved by 8 */
    .cfi_rel_offset %rbp, 0                      /* rbp is saved at CFA-relative offset 0 */
    pushq %rbx                                   /* callee-saved rbx */
    .cfi_adjust_cfa_offset 8
    .cfi_rel_offset %rbx, 0
    pushq %r12                                   /* callee-saved r12 (entry on first switch) */
    .cfi_adjust_cfa_offset 8
    .cfi_rel_offset %r12, 0
    pushq %r13                                   /* callee-saved r13 (arg on first switch) */
    .cfi_adjust_cfa_offset 8
    .cfi_rel_offset %r13, 0
    pushq %r14                                   /* callee-saved r14 */
    .cfi_adjust_cfa_offset 8
    .cfi_rel_offset %r14, 0
    pushq %r15                                   /* callee-saved r15 */
    .cfi_adjust_cfa_offset 8
    .cfi_rel_offset %r15, 0
    subq $8, %rsp                                /* reserve 8 bytes for the FP control state */
    .cfi_adjust_cfa_offset 8
    stmxcsr (%rsp)                               /* store SSE control/status (rounding, masks) */
    fnstcw 4(%rsp)                               /* store x87 control word (precision, rounding) */

    /* --- swap stacks: publish our sp, adopt the target's sp --- */
    movq %rsp, (%rdi)                            /* *from_sp = rsp (where to resume us later) */
    movq %rsi, %rsp                              /* rsp = to_sp (target's saved frame) */

    /* --- restore the target's callee-saved state from its stack --- */
    ldmxcsr (%rsp)                               /* load SSE control/status */
    fldcw 4(%rsp)                                /* load x87 control word */
    addq $8, %rsp                                /* drop the FP control slot */
    .cfi_adjust_cfa_offset -8
    popq %r15                                    /* restore r15 */
    .cfi_adjust_cfa_offset -8
    popq %r14                                    /* restore r14 */
    .cfi_adjust_cfa_offset -8
    popq %r13                                    /* restore r13 (arg for a fresh context) */
    .cfi_adjust_cfa_offset -8
    popq %r12                                    /* restore r12 (entry for a fresh context) */
    .cfi_adjust_cfa_offset -8
    popq %rbx                                    /* restore rbx */
    .cfi_adjust_cfa_offset -8
    popq %rbp                                    /* restore frame pointer */
    .cfi_adjust_cfa_offset -8
    ret                                          /* pop return address: continue the target
                                                    (its own caller of uvent_fctx_switch, or
                                                    uvent_fctx_trampoline for a fresh context) */
    .cfi_endproc                                 /* end unwind info */
    SIZE_FUNC(SYM(uvent_fctx_switch))

    .globl SYM(uvent_fctx_trampoline)
    TYPE_FUNC(SYM(uvent_fctx_trampoline))
    .p2align 4
SYM(uvent_fctx_trampoline):
    .cfi_startproc
    /* Terminate unwinding here: there is no caller frame on a fiber stack. */
    .cfi_undefined %rip                          /* tells unwinders/debuggers the backtrace ends */
    movq %r13, %rdi                              /* first argument = arg (from r13) */
    callq *%r12                                  /* call entry(arg); rsp is 16-byte aligned here
                                                    because create() left it 8 mod 16 before ret */
    ud2                                          /* entry must never return: trap if it does */
    .cfi_endproc
    SIZE_FUNC(SYM(uvent_fctx_trampoline))

#endif /* __x86_64__ */

#if defined(__ELF__)
    .section .note.GNU-stack, "", %progbits      /* ask the linker for a non-executable stack */
#endif
