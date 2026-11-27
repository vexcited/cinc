// void *cinc__switch(void **from, void *to, void *arg)
// saves the callee saved registers, mxcsr and the x87 control word on the current stack, stores its top in
// *from, loads `to` and returns `arg` there. the saved frame has the same shape on every stack, so the unwind info
// below stays true across the swap.

#if defined(__x86_64__) && defined(_WIN32)

// win64 also keeps rdi, rsi and xmm6 to xmm15, and takes the arguments in rcx, rdx and r8. the bounds of the
// running stack are in the TEB, which gs points at: StackBase at 0x08, StackLimit at 0x10 and DeallocationStack at
// 0x1478. exception dispatch, unwinding and stack probes trust them, so they are saved with the registers and
// travel with the stack, to whichever thread resumes it, as SwitchToFiber does.

  .text

  .globl cinc__switch
  .def cinc__switch
  .scl 2
  .type 32
  .endef
  .p2align 4
cinc__switch:
  .seh_proc cinc__switch
  pushq %rbp
  .seh_pushreg %rbp
  pushq %rbx
  .seh_pushreg %rbx
  pushq %rdi
  .seh_pushreg %rdi
  pushq %rsi
  .seh_pushreg %rsi
  pushq %r12
  .seh_pushreg %r12
  pushq %r13
  .seh_pushreg %r13
  pushq %r14
  .seh_pushreg %r14
  pushq %r15
  .seh_pushreg %r15
  subq $200, %rsp
  .seh_stackalloc 200
  movaps %xmm6, 0(%rsp)
  .seh_savexmm %xmm6, 0
  movaps %xmm7, 16(%rsp)
  .seh_savexmm %xmm7, 16
  movaps %xmm8, 32(%rsp)
  .seh_savexmm %xmm8, 32
  movaps %xmm9, 48(%rsp)
  .seh_savexmm %xmm9, 48
  movaps %xmm10, 64(%rsp)
  .seh_savexmm %xmm10, 64
  movaps %xmm11, 80(%rsp)
  .seh_savexmm %xmm11, 80
  movaps %xmm12, 96(%rsp)
  .seh_savexmm %xmm12, 96
  movaps %xmm13, 112(%rsp)
  .seh_savexmm %xmm13, 112
  movaps %xmm14, 128(%rsp)
  .seh_savexmm %xmm14, 128
  movaps %xmm15, 144(%rsp)
  .seh_savexmm %xmm15, 144
  .seh_endprologue
  stmxcsr 160(%rsp)
  fnstcw 164(%rsp)
  movq %gs:0x08, %rax
  movq %rax, 168(%rsp)
  movq %gs:0x10, %rax
  movq %rax, 176(%rsp)
  movq %gs:0x1478, %rax
  movq %rax, 184(%rsp)
  movq %rsp, (%rcx)
  movq %rdx, %rsp
  movq 168(%rsp), %rax
  movq %rax, %gs:0x08
  movq 176(%rsp), %rax
  movq %rax, %gs:0x10
  movq 184(%rsp), %rax
  movq %rax, %gs:0x1478
  ldmxcsr 160(%rsp)
  fldcw 164(%rsp)
  movaps 0(%rsp), %xmm6
  movaps 16(%rsp), %xmm7
  movaps 32(%rsp), %xmm8
  movaps 48(%rsp), %xmm9
  movaps 64(%rsp), %xmm10
  movaps 80(%rsp), %xmm11
  movaps 96(%rsp), %xmm12
  movaps 112(%rsp), %xmm13
  movaps 128(%rsp), %xmm14
  movaps 144(%rsp), %xmm15
  movq %r8, %rax
  .seh_startepilogue
  addq $200, %rsp
  popq %r15
  popq %r14
  popq %r13
  popq %r12
  popq %rsi
  popq %rdi
  popq %rbx
  popq %rbp
  .seh_endepilogue
  ret
  .seh_endproc

// the first frame of every fiber: rbx is the entry, r12 its fiber and rax the thread that switched in. it makes the
// shadow space of the call, and its unwind info reads a null return address at the top of the stack, where an
// unwinder stops, and gives what no frame handled to cinc__unhandled.
  .globl cinc__trampoline
  .def cinc__trampoline
  .scl 2
  .type 32
  .endef
  .p2align 4
cinc__trampoline:
  .seh_proc cinc__trampoline
  .seh_handler cinc__unhandled, @except
  subq $32, %rsp
  .seh_stackalloc 32
  .seh_endprologue
  xorl %ebp, %ebp
  movq %r12, %rcx
  movq %rax, %rdx
  callq *%rbx
  ud2
  .seh_endproc

#elif defined(__x86_64__)

#if defined(__APPLE__)
#define SYM(name) _##name
#define TYPE(name)
#define SIZE(name)
#else
#define SYM(name) name
#define TYPE(name) .type name, @function
#define SIZE(name) .size name, . - name
#endif

#if defined(__CET__)
#define LANDING endbr64
#else
#define LANDING
#endif

  .text

  .globl SYM(cinc__switch)
  TYPE(cinc__switch)
  .p2align 4
SYM(cinc__switch):
  .cfi_startproc
  LANDING
  pushq %rbp
  .cfi_adjust_cfa_offset 8
  .cfi_rel_offset %rbp, 0
  pushq %rbx
  .cfi_adjust_cfa_offset 8
  .cfi_rel_offset %rbx, 0
  pushq %r12
  .cfi_adjust_cfa_offset 8
  .cfi_rel_offset %r12, 0
  pushq %r13
  .cfi_adjust_cfa_offset 8
  .cfi_rel_offset %r13, 0
  pushq %r14
  .cfi_adjust_cfa_offset 8
  .cfi_rel_offset %r14, 0
  pushq %r15
  .cfi_adjust_cfa_offset 8
  .cfi_rel_offset %r15, 0
  subq $8, %rsp
  .cfi_adjust_cfa_offset 8
  stmxcsr (%rsp)
  fnstcw 4(%rsp)
  movq %rsp, (%rdi)
  movq %rsi, %rsp
  ldmxcsr (%rsp)
  fldcw 4(%rsp)
  addq $8, %rsp
  .cfi_adjust_cfa_offset -8
  popq %r15
  .cfi_adjust_cfa_offset -8
  .cfi_restore %r15
  popq %r14
  .cfi_adjust_cfa_offset -8
  .cfi_restore %r14
  popq %r13
  .cfi_adjust_cfa_offset -8
  .cfi_restore %r13
  popq %r12
  .cfi_adjust_cfa_offset -8
  .cfi_restore %r12
  popq %rbx
  .cfi_adjust_cfa_offset -8
  .cfi_restore %rbx
  popq %rbp
  .cfi_adjust_cfa_offset -8
  .cfi_restore %rbp
  movq %rdx, %rax
  ret
  .cfi_endproc
  SIZE(cinc__switch)

// the first frame of every fiber: rbx is the entry, r12 its fiber and rax the thread that switched in. the
// return address is undefined here and the frame chain ends, so unwinders and debuggers stop at the task.
  .globl SYM(cinc__trampoline)
  TYPE(cinc__trampoline)
  .p2align 4
SYM(cinc__trampoline):
  .cfi_startproc
  .cfi_undefined %rip
  LANDING
  xorl %ebp, %ebp
  movq %r12, %rdi
  movq %rax, %rsi
  callq *%rbx
  ud2
  .cfi_endproc
  SIZE(cinc__trampoline)

#if !defined(__APPLE__)
  .section .note.GNU-stack, "", @progbits
#endif

#endif
