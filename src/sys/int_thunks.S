.altmacro

.macro THUNK num
interrupt_thunk_\num:
.if \num != 8 && \num != 10 && \num != 11 && \num != 12 && \num != 13 && \num != 14 && \num != 17 && \num != 30
    push $0
.endif

    cmpq $0x4b, 16(%rsp) // if user
    jne 1f
    swapgs

1:
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

    mov $\num, %rdi
    mov $(\num * 8), %rax
    lea isr(%rip), %rbx
    add %rax, %rbx
    mov %rsp, %rsi
    xor %rbp, %rbp
    call *(%rbx)

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
    pop %r10
    pop %r11
    pop %r12
    pop %r13
    pop %r14
    pop %r15
    add $8, %rsp

    cmpq $0x4b, 8(%rsp) // if user
    jne 1f
    swapgs

1:
    iretq
.endm

.macro THUNKADDR num
    .quad interrupt_thunk_\num
.endm

.section .data

.global isr_thunks
.align 8
isr_thunks:
.set i,0
.rept 256
    THUNKADDR %i
    .set i,i+1
.endr

.section .text

.set i,0
.rept 256
    THUNK %i
    .set i,i+1
.endr

.global panic_ipi_entry
panic_ipi_entry:
    cli
    lock incq panic_cpu_counter(%rip)
 .hcf:
    hlt
    jmp .hcf
