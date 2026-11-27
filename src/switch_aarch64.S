// void *cinc__switch(void **from, void *to, void *arg)
// saves the callee saved registers on the current stack, stores its top in *from, loads `to` and returns `arg`
// there. the saved frame has the same shape on every stack, so the unwind info below stays true across the swap.

#if defined(__aarch64__) && defined(_WIN32)

// windows keeps the bounds of the running stack in the TEB, which x18 points at: StackBase at 0x08, StackLimit at
// 0x10 and DeallocationStack at 0x1478. exception dispatch, unwinding and stack probes trust them, so they are saved
// with the registers and travel with the stack, to whichever thread resumes it, as SwitchToFiber does. x18 itself
// is never touched.

  .text

  .globl cinc__switch
  .def cinc__switch
  .scl 2
  .type 32
  .endef
  .p2align 2
cinc__switch:
  .seh_proc cinc__switch
  sub sp, sp, #192
  .seh_stackalloc 192
  stp x19, x20, [sp, #0]
  .seh_save_regp x19, 0
  stp x21, x22, [sp, #16]
  .seh_save_regp x21, 16
  stp x23, x24, [sp, #32]
  .seh_save_regp x23, 32
  stp x25, x26, [sp, #48]
  .seh_save_regp x25, 48
  stp x27, x28, [sp, #64]
  .seh_save_regp x27, 64
  stp x29, x30, [sp, #80]
  .seh_save_fplr 80
  stp d8, d9, [sp, #96]
  .seh_save_fregp d8, 96
  stp d10, d11, [sp, #112]
  .seh_save_fregp d10, 112
  stp d12, d13, [sp, #128]
  .seh_save_fregp d12, 128
  stp d14, d15, [sp, #144]
  .seh_save_fregp d14, 144
  .seh_endprologue
  ldp x9, x10, [x18, #0x08]
  ldr x11, [x18, #0x1478]
  stp x9, x10, [sp, #160]
  str x11, [sp, #176]
  mov x9, sp
  str x9, [x0]
  mov sp, x1
  ldp x9, x10, [sp, #160]
  ldr x11, [sp, #176]
  stp x9, x10, [x18, #0x08]
  str x11, [x18, #0x1478]
  mov x0, x2
  .seh_startepilogue
  ldp d14, d15, [sp, #144]
  .seh_save_fregp d14, 144
  ldp d12, d13, [sp, #128]
  .seh_save_fregp d12, 128
  ldp d10, d11, [sp, #112]
  .seh_save_fregp d10, 112
  ldp d8, d9, [sp, #96]
  .seh_save_fregp d8, 96
  ldp x29, x30, [sp, #80]
  .seh_save_fplr 80
  ldp x27, x28, [sp, #64]
  .seh_save_regp x27, 64
  ldp x25, x26, [sp, #48]
  .seh_save_regp x25, 48
  ldp x23, x24, [sp, #32]
  .seh_save_regp x23, 32
  ldp x21, x22, [sp, #16]
  .seh_save_regp x21, 16
  ldp x19, x20, [sp, #0]
  .seh_save_regp x19, 0
  add sp, sp, #192
  .seh_stackalloc 192
  .seh_endepilogue
  ret
  .seh_endfunclet
  .seh_endproc

// the first frame of every fiber, entered with sp at the top of the stack: x19 is the entry, x20 its fiber and x0
// the thread that switched in. its unwind info reads a null return address at the top of the stack, where an
// unwinder stops, and gives what no frame handled to cinc__unhandled.
  .globl cinc__trampoline
  .def cinc__trampoline
  .scl 2
  .type 32
  .endef
  .p2align 2
cinc__trampoline:
  .seh_proc cinc__trampoline
  .seh_handler cinc__unhandled, @except
  mov x30, xzr
  .seh_nop
  stp x29, x30, [sp, #-16]!
  .seh_save_fplr_x 16
  mov x29, sp
  .seh_set_fp
  .seh_endprologue
  mov x1, x0
  mov x0, x20
  blr x19
  brk #0x1
  .seh_endfunclet
  .seh_endproc

#elif defined(__aarch64__)

#if defined(__APPLE__)
#define SYM(name) _##name
#define TYPE(name)
#define SIZE(name)
#else
#define SYM(name) name
#define TYPE(name) .type name, %function
#define SIZE(name) .size name, . - name
#endif

#if defined(__ARM_FEATURE_BTI_DEFAULT)
#define LANDING hint #34
#else
#define LANDING
#endif

  .text

  .globl SYM(cinc__switch)
  TYPE(cinc__switch)
  .p2align 2
SYM(cinc__switch):
  .cfi_startproc
  LANDING
  sub sp, sp, #160
  .cfi_def_cfa_offset 160
  stp x19, x20, [sp, #0]
  stp x21, x22, [sp, #16]
  stp x23, x24, [sp, #32]
  stp x25, x26, [sp, #48]
  stp x27, x28, [sp, #64]
  stp x29, x30, [sp, #80]
  .cfi_offset x19, -160
  .cfi_offset x20, -152
  .cfi_offset x21, -144
  .cfi_offset x22, -136
  .cfi_offset x23, -128
  .cfi_offset x24, -120
  .cfi_offset x25, -112
  .cfi_offset x26, -104
  .cfi_offset x27, -96
  .cfi_offset x28, -88
  .cfi_offset x29, -80
  .cfi_offset x30, -72
  stp d8, d9, [sp, #96]
  stp d10, d11, [sp, #112]
  stp d12, d13, [sp, #128]
  stp d14, d15, [sp, #144]
  mov x9, sp
  str x9, [x0]
  mov sp, x1
  ldp d8, d9, [sp, #96]
  ldp d10, d11, [sp, #112]
  ldp d12, d13, [sp, #128]
  ldp d14, d15, [sp, #144]
  ldp x19, x20, [sp, #0]
  ldp x21, x22, [sp, #16]
  ldp x23, x24, [sp, #32]
  ldp x25, x26, [sp, #48]
  ldp x27, x28, [sp, #64]
  ldp x29, x30, [sp, #80]
  add sp, sp, #160
  .cfi_def_cfa_offset 0
  .cfi_restore x19
  .cfi_restore x20
  .cfi_restore x21
  .cfi_restore x22
  .cfi_restore x23
  .cfi_restore x24
  .cfi_restore x25
  .cfi_restore x26
  .cfi_restore x27
  .cfi_restore x28
  .cfi_restore x29
  .cfi_restore x30
  mov x0, x2
  ret
  .cfi_endproc
  SIZE(cinc__switch)

// the first frame of every fiber: x19 is the entry, x20 its fiber and x0 the thread that switched in. the
// return address is undefined here and the frame chain ends, so unwinders and debuggers stop at the task.
  .globl SYM(cinc__trampoline)
  TYPE(cinc__trampoline)
  .p2align 2
SYM(cinc__trampoline):
  .cfi_startproc
  .cfi_undefined x30
  LANDING
  mov x29, xzr
  mov x1, x0
  mov x0, x20
  blr x19
  brk #0x1
  .cfi_endproc
  SIZE(cinc__trampoline)

#if !defined(__APPLE__)
  .section .note.GNU-stack, "", %progbits
#endif

#if defined(__linux__) && (defined(__ARM_FEATURE_BTI_DEFAULT) || defined(__ARM_FEATURE_PAC_DEFAULT))
  .pushsection .note.gnu.property, "a"
  .p2align 3
  .word 4
  .word 16
  .word 5
  .asciz "GNU"
  .word 0xc0000000
  .word 4
#if defined(__ARM_FEATURE_BTI_DEFAULT) && defined(__ARM_FEATURE_PAC_DEFAULT)
  .word 3
#elif defined(__ARM_FEATURE_BTI_DEFAULT)
  .word 1
#else
  .word 2
#endif
  .word 0
  .popsection
#endif

#endif
