; hook.asm - register-preserving trampoline for UiaReturnRawElementProvider.
;
; On entry the caller's callee-saved registers (rdi, rsi, rbx, rbp, r12-r15)
; are still intact.  We snapshot them, hand the snapshot to HookHandler, then
; restore everything and tail-jump to the real function.
;
; Windows x64: RSP is 8 mod 16 on entry (return address already pushed).

extern HookHandler:proc
extern g_realReturn:qword

.code

HookEntry proc
    sub  rsp, 88h              ; 20h shadow + 60h save area + 8 align (0x88 = 8 mod 16)

    mov  [rsp+60h], rcx        ; hwnd
    mov  [rsp+68h], rdx        ; wParam
    mov  [rsp+70h], r8         ; lParam
    mov  [rsp+78h], r9         ; provider
    mov  rax, [rsp+88h]        ; return address (caller of the IAT slot)
    mov  [rsp+80h], rax
    mov  [rsp+20h], rbx
    mov  [rsp+28h], rbp
    mov  [rsp+30h], rsi
    mov  [rsp+38h], rdi        ; <-- `this` of the caller  (prize)
    mov  [rsp+40h], r12
    mov  [rsp+48h], r13
    mov  [rsp+50h], r14
    mov  [rsp+58h], r15

    lea  rcx, [rsp+20h]        ; &SavedRegs
    call HookHandler

    mov  rbx, [rsp+20h]
    mov  rbp, [rsp+28h]
    mov  rsi, [rsp+30h]
    mov  rdi, [rsp+38h]
    mov  r12, [rsp+40h]
    mov  r13, [rsp+48h]
    mov  r14, [rsp+50h]
    mov  r15, [rsp+58h]
    mov  rcx, [rsp+60h]
    mov  rdx, [rsp+68h]
    mov  r8,  [rsp+70h]
    mov  r9,  [rsp+78h]
    add  rsp, 88h

    jmp  qword ptr [g_realReturn]
HookEntry endp

end
