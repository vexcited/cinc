// void *cinc__switch(void **from, void *to, void *arg)
// saves the callee saved registers, mxcsr and the x87 control word on the current stack, stores its top in
// *from, loads `to` and returns `arg` there. the saved frame has the same shape on every stack, so the cfi below
// stays true across the swap.

#if defined(__x86_64__)

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
