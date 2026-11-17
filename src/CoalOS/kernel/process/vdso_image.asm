; The vDSO ELF image built by CMake (userspace/vdso/vdso.c); vdso.c copies it into a user-visible page.
bits 64

section .rodata
global vdso_image, vdso_image_end
vdso_image:
    incbin "vdso.so"
vdso_image_end:

section .note.GNU-stack noalloc noexec nowrite progbits
