; lift_native_call.asm 29/09/2026. void lift_native_call(Cpu* c, uint64_t target): calls a real (TPP) function with the
; arguments held in the emulated register file: rcx rdx r8 r9, xmm0-3 and up to 8 stack qwords copied from the emulated
; stack (emulated rsp points at the shadow space, args from +20h). Stores rax and xmm0 back. Cpu layout: r[16] at 0, x[16] at 128.
.code
lift_native_call PROC FRAME
    push rbx
    .pushreg rbx
    push rsi
    .pushreg rsi
    push rdi
    .pushreg rdi
    push rbp
    .pushreg rbp
    sub rsp, 104
    .allocstack 104
    .endprolog
    mov rbx, rcx
    mov rax, rdx
    mov rsi, [rbx + 32]
    add rsi, 20h
    lea rdi, [rsp + 20h]
    mov ecx, 8
    rep movsq
    movaps xmm0, xmmword ptr [rbx + 128]
    movaps xmm1, xmmword ptr [rbx + 144]
    movaps xmm2, xmmword ptr [rbx + 160]
    movaps xmm3, xmmword ptr [rbx + 176]
    mov rcx, [rbx + 8]
    mov rdx, [rbx + 16]
    mov r8,  [rbx + 64]
    mov r9,  [rbx + 72]
    call rax
    mov [rbx], rax
    movaps xmmword ptr [rbx + 128], xmm0
    add rsp, 104
    pop rbp
    pop rdi
    pop rsi
    pop rbx
    ret
lift_native_call ENDP
END