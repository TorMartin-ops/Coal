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

file(STRINGS ${CMAKE_SOURCE_DIR}/VERSION COAL_VERSION LIMIT_COUNT 1)

set(ARCH_DIR ${CMAKE_SOURCE_DIR}/src/arch/i386)

set(FREESTANDING_FLAGS
    -m32 -march=i386 -ffreestanding -nostdinc -fno-builtin
    -fno-stack-protector -fno-pie -fno-pic
    -mno-mmx -mno-sse -mno-sse2 -mno-red-zone
    -O2 -g -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter -Werror)

execute_process(COMMAND ${CMAKE_C_COMPILER} -m32 -print-libgcc-file-name
                OUTPUT_VARIABLE LIBGCC OUTPUT_STRIP_TRAILING_WHITESPACE)

# --- Kernel -----------------------------------------------------------------
file(GLOB_RECURSE KERNEL_SOURCES CONFIGURE_DEPENDS
    boot/*.asm kernel/*.c kernel/*.asm)

# The vDSO is a tiny shared object linked at a fixed address (vdso_abi.h); the kernel embeds the file
# (kernel/process/vdso_image.asm) and maps it into every process.
set(VDSO_SO ${CMAKE_BINARY_DIR}/vdso.so)
add_custom_command(
    OUTPUT ${VDSO_SO}
    COMMAND ${CMAKE_C_COMPILER} -m32 -march=i386 -O2 -fno-pic -ffreestanding -nostdlib -shared -s
            -fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra -Werror
            -I${CMAKE_SOURCE_DIR}/include
            -Wl,--hash-style=sysv -Wl,-soname=linux-vdso.so.1 -Wl,--build-id=none -Wl,-z,noexecstack
            -Wl,--version-script=${ARCH_DIR}/vdso.ver -Wl,-T${ARCH_DIR}/vdso.ld
            -o ${VDSO_SO} ${CMAKE_SOURCE_DIR}/userspace/vdso/vdso.c
    DEPENDS userspace/vdso/vdso.c include/kernel/cpu/vdso_abi.h ${ARCH_DIR}/vdso.ld ${ARCH_DIR}/vdso.ver
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Linking the vDSO")
add_custom_target(vdso DEPENDS ${VDSO_SO})

add_executable(kernel ${KERNEL_SOURCES})
add_dependencies(kernel vdso)
set_source_files_properties(kernel/process/vdso_image.asm PROPERTIES OBJECT_DEPENDS ${VDSO_SO})
set_target_properties(kernel PROPERTIES OUTPUT_NAME kernel.bin)
target_include_directories(kernel PRIVATE include kernel ${CMAKE_BINARY_DIR})
target_compile_definitions(kernel PRIVATE COAL_VERSION="${COAL_VERSION}")
target_compile_options(kernel PRIVATE $<$<COMPILE_LANGUAGE:C>:${FREESTANDING_FLAGS}>)
target_link_options(kernel PRIVATE
    -m32 -nostdlib -static -no-pie -Wl,--build-id=none -T${ARCH_DIR}/linker.ld)
target_link_libraries(kernel PRIVATE ${LIBGCC})

# --- User programs ------------------------------------------------------------
file(GLOB USER_LIBC_SOURCES CONFIGURE_DEPENDS userspace/libc/*.c)

# User programs are small: -Os, warnings are errors, no libgcc (the libc avoids
# the helpers it would provide), unused sections are dropped and the result is stripped.
set(USER_FLAGS
    -m32 -march=i386 -ffreestanding -nostdinc
    -fno-stack-protector -fno-pie -fno-pic
    -mno-mmx -mno-sse -mno-sse2 -mno-red-zone
    -Os -Wall -Wextra -Werror -Wno-unused-parameter
    -fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns
    -ffunction-sections -fdata-sections)

function(add_user_program name)
    add_executable(${name} ${ARGN} userspace/entry.asm ${USER_LIBC_SOURCES})
    set_target_properties(${name} PROPERTIES OUTPUT_NAME ${name}.elf)
    target_include_directories(${name} PRIVATE include)
    target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:C>:${USER_FLAGS}>)
    target_link_options(${name} PRIVATE
        -m32 -nostdlib -static -no-pie -s -Wl,--build-id=none
        -Wl,--gc-sections -T${ARCH_DIR}/user.ld)
endfunction()

file(GLOB COALBOX_SOURCES CONFIGURE_DEPENDS userspace/coalbox/*.c)
add_user_program(coalbox ${COALBOX_SOURCES})   # installed as /bin/coalbox
add_user_program(init userspace/init/init.c)   # installed as /sbin/init
file(GLOB SH_SOURCES CONFIGURE_DEPENDS userspace/sh/*.c)
add_user_program(shell ${SH_SOURCES} userspace/coalbox/test.c userspace/coalbox/match.c)   # installed as /bin/sh
add_user_program(hello userspace/hello/hello.c)
add_user_program(snake userspace/games/snake.c)
add_user_program(libc_test userspace/tests/libc_test.c)
add_user_program(syscall_test userspace/tests/syscall_test.c)
add_user_program(mcpd userspace/mcpd/mcpd.c)
add_user_program(memtest userspace/tests/memtest.c)
add_user_program(fault_test userspace/tests/fault_test.c)
add_user_program(fs_stress userspace/tests/fs_stress.c)
add_user_program(kernel_test userspace/tests/kernel_test.c)
add_user_program(coalbench userspace/bench/coalbench.c)
add_user_program(coalbench_null userspace/bench/null.c)

# Raw int 0x80 smoke test: no libc, no start-up code.
add_executable(abi_test userspace/tests/abi_test.c)
set_target_properties(abi_test PROPERTIES OUTPUT_NAME abi_test.elf)
target_compile_options(abi_test PRIVATE -m32 -ffreestanding -nostdlib -fno-pie -fno-pic
    -fno-stack-protector -mno-sse -O1 -Wall -Wextra -Werror)
target_link_options(abi_test PRIVATE -m32 -nostdlib -static -no-pie -s -Wl,--build-id=none
    -T${ARCH_DIR}/user.ld)

# --- Disk image (FAT16, long file names) with a Linux directory layout ---------
# Static files for the image live in rootfs/ in the source tree.
set(ROOTFS_DIR ${CMAKE_SOURCE_DIR}/rootfs)
file(GLOB ROOTFS_ETC CONFIGURE_DEPENDS ${ROOTFS_DIR}/etc/*)

set(DISK_IMAGE ${CMAKE_BINARY_DIR}/disk.img)
set(IMG -i ${DISK_IMAGE})
add_custom_command(
    OUTPUT ${DISK_IMAGE}
    COMMAND rm -f ${DISK_IMAGE}
    COMMAND mkfs.fat -C -F 16 ${DISK_IMAGE} 32768
    COMMAND mmd ${IMG} ::/bin ::/sbin ::/etc ::/home ::/root ::/tmp ::/dev ::/proc ::/usr ::/usr/bin ::/var
    COMMAND mcopy ${IMG} $<TARGET_FILE:shell> ::/bin/sh
    COMMAND mcopy ${IMG} $<TARGET_FILE:coalbox> ::/bin/coalbox
    COMMAND mcopy ${IMG} $<TARGET_FILE:init> ::/sbin/init
    COMMAND mcopy ${IMG} $<TARGET_FILE:mcpd> ::/sbin/mcpd
    COMMAND mcopy ${IMG} $<TARGET_FILE:hello> ::/usr/bin/hello
    COMMAND mcopy ${IMG} $<TARGET_FILE:snake> ::/usr/bin/snake
    COMMAND mcopy ${IMG} $<TARGET_FILE:libc_test> ::/usr/bin/libc_test
    COMMAND mcopy ${IMG} $<TARGET_FILE:syscall_test> ::/usr/bin/syscall_test
    COMMAND mcopy ${IMG} $<TARGET_FILE:abi_test> ::/usr/bin/abi_test
    COMMAND mcopy ${IMG} $<TARGET_FILE:memtest> ::/usr/bin/memtest
    COMMAND mcopy ${IMG} $<TARGET_FILE:fault_test> ::/usr/bin/fault_test
    COMMAND mcopy ${IMG} $<TARGET_FILE:fs_stress> ::/usr/bin/fs_stress
    COMMAND mcopy ${IMG} $<TARGET_FILE:kernel_test> ::/usr/bin/kernel_test
    COMMAND mcopy ${IMG} $<TARGET_FILE:coalbench> ::/usr/bin/coalbench
    COMMAND mcopy ${IMG} $<TARGET_FILE:coalbench_null> ::/usr/bin/coalbench_null
    COMMAND mcopy ${IMG} ${CMAKE_CURRENT_SOURCE_DIR}/userspace/tests/sh_test.sh ::/usr/bin/sh_test.sh
    COMMAND mcopy ${IMG} ${CMAKE_CURRENT_SOURCE_DIR}/userspace/tests/applet_test.sh ::/usr/bin/applet_test.sh
    COMMAND mcopy ${IMG} ${ROOTFS_ETC} ::/etc/
    DEPENDS hello snake shell coalbox init mcpd libc_test syscall_test abi_test memtest fault_test fs_stress kernel_test coalbench coalbench_null ${ROOTFS_ETC}
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
# Kernel only; leaves the disk image (and the files on it) untouched.
add_custom_target(iso DEPENDS ${ISO_IMAGE})

set(COAL_MEMORY_MB 512 CACHE STRING "Guest RAM in MiB for the run target")
add_custom_target(run
    COMMAND qemu-system-i386 -cdrom ${ISO_IMAGE} -hda ${DISK_IMAGE}
            -boot d -m ${COAL_MEMORY_MB}M -serial stdio
            -nic user,model=e1000
    DEPENDS image
    USES_TERMINAL)
