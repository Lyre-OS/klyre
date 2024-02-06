#include <errno.h>

.macro THUNK
    swapgs
    push $0

    push %r15
    push %r14
    push %r13
    push %r12
    push %r11
    push %r10
    push %r9
    push %r8
    push %rbp
    push %rdi
    push %rsi
    push %rdx
    push %rcx
    push %rbx
    push %rax

    mov %es, %eax
    push %rax
    mov %ds, %eax
    push %rax

    cld

    mov $0x30, %eax
    mov %eax, %ds
    mov %eax, %es
    mov %eax, %ss

    mov %rdi, %rbx
    mov %rsp, %rdi

    movq $-1, 16(%rsp)
    movq $EINVAL, 24(%rsp)

    cmp syscall_count(%rip), %rbx
    jae 1f

    sti
    lea syscall_table(%rip), %rax
    call *(%rax,%rbx,8)

    mov %rax, 16(%rsp)
    mov %gs:0x8, %rax
    mov %rax, 24(%rsp)

    cli

1:
    pop %rax
    mov %eax, %ds
    pop %rax
    mov %eax, %es

    pop %rax
    pop %rbx
    pop %rcx
    pop %rdx
    pop %rsi
    pop %rdi
    pop %rbp
    pop %r8
    pop %r9
    pop %rcx
    pop %rdx
    pop %r12
    pop %r13
    pop %r14
    pop %r15

    swapgs
.endm

.section .text

.global syscall_ud_entry
syscall_ud_entry:
    push %rax
    mov 0x8(%rsp), %rax
    cmpw $0x340f, (%rax)
    pop %rax
    jne 2f
    addq $2, (%rsp)
    THUNK
    add $8, %rsp
    iretq
2:
    jmp *(isr_thunks + 0x6 * 0x8)(%rip)

.global syscall_sysenter_entry
syscall_sysenter_entry:
    push $0x53
    push %r10
    pushfq
    orq $0x200, (%rsp)
    push $0x4b
    push %r11

    THUNK

    sti
    sysexitq
