; Native x86-64 side of the differential harness.
; run_case(code, out): calls a generated test body (which ends in `ret` in native builds),
; then stores RAX..R15 (RSP slot left zero) and XMM0..15 into `out`.
; Layout of out: 16 x u64 GPRs in FEX order (RAX RBX RCX RDX RSI RDI RBP RSP R8..R15),
; then 16 x 16-byte XMM registers.
default rel
bits 64

section .bss
out_ptr:     resq 1
saved_mxcsr: resd 1

section .text
global run_case
run_case:
  push rbx
  push rbp
  push r12
  push r13
  push r14
  push r15
  sub rsp, 8                  ; body sees a normal, 16-byte aligned call frame
  mov [out_ptr], rsi
  stmxcsr [saved_mxcsr]
  call rdi

  push rax
  mov rax, [out_ptr]
  mov [rax + 8*1], rbx
  mov [rax + 8*2], rcx
  mov [rax + 8*3], rdx
  mov [rax + 8*4], rsi
  mov [rax + 8*5], rdi
  mov [rax + 8*6], rbp
  mov qword [rax + 8*7], 0
  mov [rax + 8*8], r8
  mov [rax + 8*9], r9
  mov [rax + 8*10], r10
  mov [rax + 8*11], r11
  mov [rax + 8*12], r12
  mov [rax + 8*13], r13
  mov [rax + 8*14], r14
  mov [rax + 8*15], r15
  pop rbx
  mov [rax], rbx
%assign i 0
%rep 16
  movdqu [rax + 128 + 16*i], xmm %+ i
%assign i i+1
%endrep
  ldmxcsr [saved_mxcsr]

  add rsp, 8
  pop r15
  pop r14
  pop r13
  pop r12
  pop rbp
  pop rbx
  ret

section .note.GNU-stack noalloc noexec nowrite progbits
