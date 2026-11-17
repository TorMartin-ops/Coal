; uint32_t *setup_idle_context(uint32_t *stack_top, void (*idle_func)(void))
;
; Build the frame simple_switch expects to pop (gs, fs, es, ds, pushad
; registers, eflags, return address) so the idle task can be switched to.
; Returns the new stack pointer.

%include "kernel/cpu/cpu_defs.inc"

section .text
global setup_idle_context

setup_idle_context:
    mov     edx, [esp + 4]          ; stack_top
    mov     ecx, [esp + 8]          ; idle_func
    sub     edx, 4
    mov     [edx], ecx              ; return address for simple_switch's ret
    sub     edx, 4
    mov     dword [edx], 0x202      ; eflags: IF=1
    sub     edx, 32                 ; pushad area: all registers zero
    mov     dword [edx +  0], 0
    mov     dword [edx +  4], 0
    mov     dword [edx +  8], 0
    mov     dword [edx + 12], 0
    mov     dword [edx + 16], 0
    mov     dword [edx + 20], 0
    mov     dword [edx + 24], 0
    mov     dword [edx + 28], 0
    sub     edx, 16                 ; segment registers
    mov     dword [edx +  0], KERNEL_DS
    mov     dword [edx +  4], KERNEL_DS
    mov     dword [edx +  8], KERNEL_DS
    mov     dword [edx + 12], KERNEL_DS
    mov     eax, edx
    ret
