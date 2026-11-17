cmake_minimum_required(VERSION 3.22)

# Use the i686-elf cross compiler when it is installed; otherwise fall back to
# the host GCC in 32-bit mode (works on any x86-64 Linux with gcc-multilib).
find_program(CROSS_CC i686-elf-gcc)
if(CROSS_CC)
    set(CMAKE_C_COMPILER ${CROSS_CC})
else()
    set(CMAKE_C_COMPILER gcc)
endif()
set(CMAKE_C_COMPILER_WORKS 1)

project(CoalOS LANGUAGES C ASM_NASM)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_ASM_NASM_OBJECT_FORMAT elf32)
set(CMAKE_ASM_NASM_COMPILE_OBJECT
    "<CMAKE_ASM_NASM_COMPILER> <INCLUDES> <FLAGS> -f elf32 -g -o <OBJECT> <SOURCE>")

set(ARCH_DIR ${CMAKE_SOURCE_DIR}/src/arch/i386)

set(FREESTANDING_FLAGS
    -m32 -march=i386 -ffreestanding -nostdinc -fno-builtin
    -fno-stack-protector -fno-pie -fno-pic
    -mno-mmx -mno-sse -mno-sse2 -mno-red-zone
    -O0 -g -Wall -Wextra -Wno-unused-parameter)

execute_process(COMMAND ${CMAKE_C_COMPILER} -m32 -print-libgcc-file-name
                OUTPUT_VARIABLE LIBGCC OUTPUT_STRIP_TRAILING_WHITESPACE)

# --- Kernel -----------------------------------------------------------------
file(GLOB_RECURSE KERNEL_SOURCES CONFIGURE_DEPENDS
    boot/*.asm kernel/*.c kernel/*.asm)

add_executable(kernel ${KERNEL_SOURCES})
set_target_properties(kernel PROPERTIES OUTPUT_NAME kernel.bin)
target_include_directories(kernel PRIVATE include kernel)
target_compile_options(kernel PRIVATE $<$<COMPILE_LANGUAGE:C>:${FREESTANDING_FLAGS}>)
target_link_options(kernel PRIVATE
    -m32 -nostdlib -static -no-pie -Wl,--build-id=none -T${ARCH_DIR}/linker.ld)
target_link_libraries(kernel PRIVATE ${LIBGCC})

# --- User programs ------------------------------------------------------------
file(GLOB USER_LIBC_SOURCES CONFIGURE_DEPENDS userspace/libc/*.c)

# Same flags as the kernel, but warnings are errors for user programs.
function(add_user_program name)
    add_executable(${name} ${ARGN} userspace/entry.asm ${USER_LIBC_SOURCES})
    set_target_properties(${name} PROPERTIES OUTPUT_NAME ${name}.elf)
    target_include_directories(${name} PRIVATE include)
    target_compile_options(${name} PRIVATE
        $<$<COMPILE_LANGUAGE:C>:${FREESTANDING_FLAGS}>
        $<$<COMPILE_LANGUAGE:C>:-Werror>)
    target_link_options(${name} PRIVATE
        -m32 -nostdlib -static -no-pie -Wl,--build-id=none -Wl,--no-warn-rwx-segments
        -T${ARCH_DIR}/user.ld)
    target_link_libraries(${name} PRIVATE ${LIBGCC})
endfunction()

add_user_program(hello userspace/hello/hello.c)
file(GLOB SHELL_SOURCES CONFIGURE_DEPENDS userspace/shell/shell_main.c userspace/shell/modules/*.c)
add_user_program(shell ${SHELL_SOURCES})
add_user_program(libc_test userspace/tests/libc_test.c)
add_user_program(syscall_test userspace/tests/syscall_test.c)
add_user_program(mcpd userspace/mcpd/mcpd.c)

# --- Disk image (FAT16) with the user programs --------------------------------
set(DISK_IMAGE ${CMAKE_BINARY_DIR}/disk.img)
add_custom_command(
    OUTPUT ${DISK_IMAGE}
    COMMAND rm -f ${DISK_IMAGE}
    COMMAND mkfs.fat -C -F 16 ${DISK_IMAGE} 32768
    COMMAND mcopy -i ${DISK_IMAGE} $<TARGET_FILE:hello> ::/hello.elf
    COMMAND mmd -i ${DISK_IMAGE} ::/bin
    COMMAND mcopy -i ${DISK_IMAGE} $<TARGET_FILE:shell> ::/bin/shell.elf
    COMMAND mcopy -i ${DISK_IMAGE} $<TARGET_FILE:libc_test> ::/bin/libc_test.elf
    COMMAND mcopy -i ${DISK_IMAGE} $<TARGET_FILE:syscall_test> ::/bin/syscall_test.elf
    DEPENDS hello shell libc_test syscall_test
    COMMENT "Creating FAT16 disk image"
    VERBATIM)

# --- Bootable ISO (GRUB, Multiboot2) ------------------------------------------
set(ISO_DIR ${CMAKE_BINARY_DIR}/iso)
set(ISO_IMAGE ${CMAKE_BINARY_DIR}/kernel.iso)
add_custom_command(
    OUTPUT ${ISO_IMAGE}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${ISO_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ISO_DIR}/boot/grub
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:kernel> ${ISO_DIR}/boot/kernel.bin
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_SOURCE_DIR}/boot/grub.cfg ${ISO_DIR}/boot/grub/grub.cfg
    COMMAND grub-mkrescue -o ${ISO_IMAGE} ${ISO_DIR}
    DEPENDS kernel ${CMAKE_SOURCE_DIR}/boot/grub.cfg
    COMMENT "Creating bootable ISO"
    VERBATIM)

add_custom_target(image ALL DEPENDS ${ISO_IMAGE} ${DISK_IMAGE})

add_custom_target(run
    COMMAND qemu-system-i386 -cdrom ${ISO_IMAGE} -hda ${DISK_IMAGE}
            -boot d -m 128M -serial stdio
    DEPENDS image
    USES_TERMINAL)
