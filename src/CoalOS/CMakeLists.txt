cmake_minimum_required(VERSION 3.22)

# Use the x86_64-elf cross compiler when it is installed; otherwise the host GCC (x86_64-linux-gnu on an
# x86-64 Linux machine, which is what the freestanding flags below are written for).
find_program(CROSS_CC x86_64-elf-gcc)
find_program(HOST_CC x86_64-linux-gnu-gcc)
if(CROSS_CC)
    set(CMAKE_C_COMPILER ${CROSS_CC})
elseif(HOST_CC)
    set(CMAKE_C_COMPILER ${HOST_CC})
else()
    set(CMAKE_C_COMPILER gcc)
endif()
set(CMAKE_C_COMPILER_WORKS 1)

project(CoalOS LANGUAGES C ASM_NASM)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_ASM_NASM_OBJECT_FORMAT elf64)
set(CMAKE_ASM_NASM_COMPILE_OBJECT
    "<CMAKE_ASM_NASM_COMPILER> <INCLUDES> <FLAGS> -f elf64 -g -o <OBJECT> <SOURCE>")

file(STRINGS ${CMAKE_SOURCE_DIR}/VERSION COAL_VERSION LIMIT_COUNT 1)

set(ARCH_DIR ${CMAKE_SOURCE_DIR}/src/arch/x86_64)

# The test profile builds a debug kernel: sanitizers, stack protector, red zones, KFENCE, lock checker and
# symbolised backtraces (docs/testing/sanitizers.md). The release profile is unchanged.
set(COAL_PROFILE "release" CACHE STRING "release or test")
if(COAL_PROFILE STREQUAL "test")
    set(COAL_DEBUG_DEFAULT ON)
else()
    set(COAL_DEBUG_DEFAULT OFF)
endif()
option(COAL_DEBUG_KERNEL "Build the kernel with sanitizers and debug checks" ${COAL_DEBUG_DEFAULT})

set(FREESTANDING_FLAGS
    -m64 -mcmodel=kernel -ffreestanding -nostdinc -fno-builtin
    -fno-stack-protector -fno-pie -fno-pic
    -mgeneral-regs-only -mno-red-zone
    -O2 -g -fno-strict-aliasing -Wall -Wextra -Wno-unused-parameter -Werror)
if(COAL_DEBUG_KERNEL)
    list(REMOVE_ITEM FREESTANDING_FLAGS -fno-stack-protector)
    list(APPEND FREESTANDING_FLAGS
        -DCOAL_DEBUG_KERNEL=1
        -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer
        -fstack-protector-strong -mstack-protector-guard=global
        -fsanitize=undefined -fno-sanitize=vptr -fno-sanitize-recover=unreachable,return)
endif()

# --- Kernel -----------------------------------------------------------------
file(GLOB_RECURSE KERNEL_SOURCES CONFIGURE_DEPENDS
    boot/*.asm kernel/*.c kernel/*.asm)
if(NOT COAL_DEBUG_KERNEL)
    list(FILTER KERNEL_SOURCES EXCLUDE REGEX "/kernel/debug/")
endif()

# The vDSO is a tiny shared object linked at a fixed address (vdso_abi.h); the kernel embeds the file
# (kernel/process/vdso_image.asm) and maps it into every process.
set(VDSO_SO ${CMAKE_BINARY_DIR}/vdso.so)
add_custom_command(
    OUTPUT ${VDSO_SO}
    COMMAND ${CMAKE_C_COMPILER} -m64 -O2 -fPIC -ffreestanding -nostdlib -shared -s
            -mgeneral-regs-only -fno-stack-protector -fno-asynchronous-unwind-tables -Wall -Wextra -Werror
            -I${CMAKE_SOURCE_DIR}/include
            -Wl,--hash-style=sysv -Wl,-soname=linux-vdso.so.1 -Wl,--build-id=none -Wl,-z,noexecstack
            -Wl,-z,max-page-size=0x1000
            -Wl,--version-script=${ARCH_DIR}/vdso.ver -Wl,-T${ARCH_DIR}/vdso.ld
            -o ${VDSO_SO} ${CMAKE_SOURCE_DIR}/userspace/vdso/vdso.c
    DEPENDS userspace/vdso/vdso.c include/kernel/cpu/vdso_abi.h ${ARCH_DIR}/vdso.ld ${ARCH_DIR}/vdso.ver
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Linking the vDSO")
add_custom_target(vdso DEPENDS ${VDSO_SO})

set(KERNEL_LINK_OPTIONS
    -m64 -nostdlib -static -no-pie -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 -Wl,-z,noexecstack
    -T${ARCH_DIR}/linker.ld)

if(COAL_DEBUG_KERNEL)
    # Two links: the first gives the function addresses, the second embeds them as the symbol table that
    # backtraces use. The table sits behind .text, so no function moves between the links (checked below).
    add_library(kernel_objs OBJECT ${KERNEL_SOURCES})
    add_dependencies(kernel_objs vdso)
    set_source_files_properties(kernel/process/vdso_image.asm PROPERTIES OBJECT_DEPENDS ${VDSO_SO})
    target_include_directories(kernel_objs PRIVATE include kernel ${CMAKE_BINARY_DIR})
    target_compile_definitions(kernel_objs PRIVATE COAL_VERSION="${COAL_VERSION}")
    target_compile_options(kernel_objs PRIVATE $<$<COMPILE_LANGUAGE:C>:${FREESTANDING_FLAGS}>)

    set(KSYMS_GEN ${CMAKE_SOURCE_DIR}/scripts/gen_ksyms.py)
    set(KSYMS_STUB ${CMAKE_BINARY_DIR}/ksyms_stub.c)
    set(KSYMS_C ${CMAKE_BINARY_DIR}/ksyms.c)
    add_custom_command(OUTPUT ${KSYMS_STUB} COMMAND python3 ${KSYMS_GEN} --stub ${KSYMS_STUB} DEPENDS ${KSYMS_GEN})
    add_executable(kernel_stage1 $<TARGET_OBJECTS:kernel_objs> ${KSYMS_STUB})
    set_target_properties(kernel_stage1 PROPERTIES LINK_DEPENDS ${ARCH_DIR}/linker.ld)
    target_compile_options(kernel_stage1 PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffreestanding -nostdinc -mcmodel=kernel -fno-pic -fno-pie>)
    target_include_directories(kernel_stage1 PRIVATE include)
    target_link_options(kernel_stage1 PRIVATE ${KERNEL_LINK_OPTIONS})
    add_custom_command(OUTPUT ${KSYMS_C}
        COMMAND python3 ${KSYMS_GEN} $<TARGET_FILE:kernel_stage1> ${KSYMS_C}
        DEPENDS kernel_stage1 ${KSYMS_GEN})
    add_executable(kernel $<TARGET_OBJECTS:kernel_objs> ${KSYMS_C})
    target_compile_options(kernel PRIVATE $<$<COMPILE_LANGUAGE:C>:-ffreestanding -nostdinc -mcmodel=kernel -fno-pic -fno-pie>)
    target_include_directories(kernel PRIVATE include)
    add_custom_command(TARGET kernel POST_BUILD
        COMMAND python3 ${KSYMS_GEN} --check $<TARGET_FILE:kernel_stage1> $<TARGET_FILE:kernel>
        COMMENT "Checking that the symbol table did not move any function")
else()
    add_executable(kernel ${KERNEL_SOURCES})
    add_dependencies(kernel vdso)
    set_source_files_properties(kernel/process/vdso_image.asm PROPERTIES OBJECT_DEPENDS ${VDSO_SO})
    target_include_directories(kernel PRIVATE include kernel ${CMAKE_BINARY_DIR})
    target_compile_definitions(kernel PRIVATE COAL_VERSION="${COAL_VERSION}")
    target_compile_options(kernel PRIVATE $<$<COMPILE_LANGUAGE:C>:${FREESTANDING_FLAGS}>)
endif()
set_target_properties(kernel PROPERTIES OUTPUT_NAME kernel.bin LINK_DEPENDS ${ARCH_DIR}/linker.ld)
target_link_options(kernel PRIVATE ${KERNEL_LINK_OPTIONS})

# --- User programs ------------------------------------------------------------
file(GLOB USER_LIBC_SOURCES CONFIGURE_DEPENDS userspace/libc/*.c)

# User programs are small: -Os, warnings are errors, no libgcc (the libc avoids
# the helpers it would provide), unused sections are dropped and the result is stripped.
set(USER_FLAGS
    -m64 -ffreestanding -nostdinc
    -fno-stack-protector -fno-pie -fno-pic
    -Os -Wall -Wextra -Werror -Wno-unused-parameter
    -fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns
    -ffunction-sections -fdata-sections)

function(add_user_program name)
    add_executable(${name} ${ARGN} userspace/entry.asm ${USER_LIBC_SOURCES})
    set_target_properties(${name} PROPERTIES OUTPUT_NAME ${name}.elf)
    target_include_directories(${name} PRIVATE include)
    target_compile_options(${name} PRIVATE $<$<COMPILE_LANGUAGE:C>:${USER_FLAGS}>)
    target_link_options(${name} PRIVATE
        -m64 -nostdlib -static -no-pie -s -Wl,--build-id=none -Wl,-z,max-page-size=0x1000 -Wl,-z,noexecstack
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
add_user_program(agentd userspace/agentd/agentd.c)
add_user_program(memtest userspace/tests/memtest.c)
add_user_program(fault_test userspace/tests/fault_test.c)
add_user_program(fs_stress userspace/tests/fs_stress.c)
add_user_program(kernel_test userspace/tests/kernel_test.c)
add_user_program(seed_test userspace/tests/seed_test.c)
add_user_program(coalbench userspace/bench/coalbench.c)
add_user_program(coalbench_null userspace/bench/null.c)
if(COAL_DEBUG_KERNEL)
    add_user_program(ksan_test userspace/tests/ksan_test.c)         # provokes sanitizer reports (debug kernel only)
    add_user_program(fuzz_executor userspace/tests/fuzz_executor.c) # guest side of tools/fuzz; the fuzzer puts it on its own image
    set(KSAN_TEST_DEP ksan_test)
endif()

# Raw SYSCALL smoke test: no libc, no start-up code.
add_executable(abi_test userspace/tests/abi_test.c)
set_target_properties(abi_test PROPERTIES OUTPUT_NAME abi_test.elf)
target_include_directories(abi_test PRIVATE include)
target_compile_options(abi_test PRIVATE -m64 -ffreestanding -nostdlib -fno-pie -fno-pic
    -fno-stack-protector -O1 -Wall -Wextra -Werror)
target_link_options(abi_test PRIVATE -m64 -nostdlib -static -no-pie -s -Wl,--build-id=none
    -Wl,-z,max-page-size=0x1000 -Wl,-z,noexecstack -T${ARCH_DIR}/user.ld)

# --- Disk image (FAT16, long file names) with a Linux directory layout ---------
# Static files for the image live in rootfs/ in the source tree.
set(ROOTFS_DIR ${CMAKE_SOURCE_DIR}/rootfs)
file(GLOB ROOTFS_ETC CONFIGURE_DEPENDS ${ROOTFS_DIR}/etc/*)

set(DISK_IMAGE ${CMAKE_BINARY_DIR}/disk.img)
set(IMG -i ${DISK_IMAGE})
if(COAL_DEBUG_KERNEL)
    set(KSAN_TEST_COPY COMMAND mcopy ${IMG} $<TARGET_FILE:ksan_test> ::/usr/bin/ksan_test)
endif()
add_custom_command(
    OUTPUT ${DISK_IMAGE}
    COMMAND rm -f ${DISK_IMAGE}
    COMMAND mkfs.fat -C -F 16 ${DISK_IMAGE} 32768
    COMMAND mmd ${IMG} ::/bin ::/sbin ::/etc ::/home ::/root ::/tmp ::/dev ::/proc ::/usr ::/usr/bin ::/var
    COMMAND mcopy ${IMG} $<TARGET_FILE:shell> ::/bin/sh
    COMMAND mcopy ${IMG} $<TARGET_FILE:coalbox> ::/bin/coalbox
    COMMAND mcopy ${IMG} $<TARGET_FILE:init> ::/sbin/init
    COMMAND mcopy ${IMG} $<TARGET_FILE:agentd> ::/sbin/agentd
    COMMAND mcopy ${IMG} $<TARGET_FILE:hello> ::/usr/bin/hello
    COMMAND mcopy ${IMG} $<TARGET_FILE:snake> ::/usr/bin/snake
    COMMAND mcopy ${IMG} $<TARGET_FILE:libc_test> ::/usr/bin/libc_test
    COMMAND mcopy ${IMG} $<TARGET_FILE:syscall_test> ::/usr/bin/syscall_test
    COMMAND mcopy ${IMG} $<TARGET_FILE:abi_test> ::/usr/bin/abi_test
    COMMAND mcopy ${IMG} $<TARGET_FILE:memtest> ::/usr/bin/memtest
    COMMAND mcopy ${IMG} $<TARGET_FILE:fault_test> ::/usr/bin/fault_test
    COMMAND mcopy ${IMG} $<TARGET_FILE:fs_stress> ::/usr/bin/fs_stress
    COMMAND mcopy ${IMG} $<TARGET_FILE:kernel_test> ::/usr/bin/kernel_test
    COMMAND mcopy ${IMG} $<TARGET_FILE:seed_test> ::/usr/bin/seed_test
    COMMAND mcopy ${IMG} $<TARGET_FILE:coalbench> ::/usr/bin/coalbench
    COMMAND mcopy ${IMG} $<TARGET_FILE:coalbench_null> ::/usr/bin/coalbench_null
    COMMAND mcopy ${IMG} ${CMAKE_CURRENT_SOURCE_DIR}/userspace/tests/sh_test.sh ::/usr/bin/sh_test.sh
    COMMAND mcopy ${IMG} ${CMAKE_CURRENT_SOURCE_DIR}/userspace/tests/applet_test.sh ::/usr/bin/applet_test.sh
    COMMAND mcopy ${IMG} ${ROOTFS_ETC} ::/etc/
    ${KSAN_TEST_COPY}
    DEPENDS ${KSAN_TEST_DEP} hello snake shell coalbox init agentd libc_test syscall_test abi_test memtest fault_test fs_stress kernel_test seed_test coalbench coalbench_null ${ROOTFS_ETC}
    COMMENT "Creating FAT16 disk image"
    VERBATIM)

# --- Bootable ISO (GRUB, Multiboot2) ------------------------------------------
# The debug kernel's ISO boots with every sanitizer finding fatal, so a test run that touches a bug fails visibly.
set(GRUB_CFG ${CMAKE_SOURCE_DIR}/boot/grub.cfg)
if(COAL_DEBUG_KERNEL)
    set(COAL_CMDLINE "coal.ubsan=panic coal.san=panic coal.lockdep=panic coal.kfence=100" CACHE STRING "Kernel command line of the debug ISO")
    set(GRUB_CFG ${CMAKE_BINARY_DIR}/grub.cfg)
    file(WRITE ${GRUB_CFG} "set timeout=0\nset default=0\n\nmenuentry \"Coal OS\" {\n    multiboot2 /boot/kernel.bin ${COAL_CMDLINE}\n    boot\n}\n")
endif()
set(ISO_DIR ${CMAKE_BINARY_DIR}/iso)
set(ISO_IMAGE ${CMAKE_BINARY_DIR}/kernel.iso)
add_custom_command(
    OUTPUT ${ISO_IMAGE}
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${ISO_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ISO_DIR}/boot/grub
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:kernel> ${ISO_DIR}/boot/kernel.bin
    COMMAND ${CMAKE_COMMAND} -E copy ${GRUB_CFG} ${ISO_DIR}/boot/grub/grub.cfg
    COMMAND grub-mkrescue -o ${ISO_IMAGE} ${ISO_DIR}
    DEPENDS kernel ${GRUB_CFG}
    COMMENT "Creating bootable ISO"
    VERBATIM)

add_custom_target(image ALL DEPENDS ${ISO_IMAGE} ${DISK_IMAGE})
# Kernel only; leaves the disk image (and the files on it) untouched.
add_custom_target(iso DEPENDS ${ISO_IMAGE})

set(COAL_MEMORY_MB 512 CACHE STRING "Guest RAM in MiB for the run target")
add_custom_target(run
    COMMAND qemu-system-x86_64 -cpu max -cdrom ${ISO_IMAGE} -hda ${DISK_IMAGE}
            -boot d -m ${COAL_MEMORY_MB}M -serial stdio
            -nic user,model=e1000
    DEPENDS image
    USES_TERMINAL)
