########################################
# The University of Agder Operating System: UiAOS
# Languages: C, C++, and NASM Assembly
# Tip: Use Ctrl+Shift+P in Visual Studio Code to get started with CMake.
########################################

# Skip compiler self-tests (saves time, avoids errors with some cross compilers)
set(CMAKE_C_COMPILER_WORKS 1)
set(CMAKE_CXX_COMPILER_WORKS 1)

# Minimum required CMake version
cmake_minimum_required(VERSION 3.22.1)

# Project name and languages used
project(UiAOS LANGUAGES C CXX ASM_NASM)

# Create a lock file to prevent parallel runs of CMake
file(LOCK ${CMAKE_SOURCE_DIR} DIRECTORY GUARD FILE)

########################################
# CMake: Import Plugins
########################################
include(FetchContent)

########################################
# UiAOS: Variables
########################################
set(OS_ARCH_TARGET "i386")  # or x86_64
set(OS_NAME "UiA Operating System")
set(OS_KERNEL_NAME "uiaos")
set(OS_KERNEL_BINARY "kernel.bin")
set(OS_KERNEL_IMAGE "kernel.iso")

########################################
# Compiler Configuration
########################################
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 99)

########################################
# Assembly Configuration
########################################
set(CMAKE_ASM_NASM_SOURCE_FILE_EXTENSIONS "s;S;asm")
if(OS_ARCH_TARGET STREQUAL "i386")
    set(CMAKE_ASM_NASM_OBJECT_FORMAT "elf32")
elseif(OS_ARCH_TARGET STREQUAL "x86_64")
    set(CMAKE_ASM_NASM_OBJECT_FORMAT "elf64")
endif()

# Command to compile NASM files
set(CMAKE_ASM_NASM_COMPILE_OBJECT
    "<CMAKE_ASM_NASM_COMPILER> <DEFINES> <INCLUDES> <FLAGS> "
    "-f ${CMAKE_ASM_NASM_OBJECT_FORMAT} -o <OBJECT> <SOURCE>"
)

########################################
# Kernel Build
########################################
# Linker script for the kernel (i386 example)
set(OS_KERNEL_LINKER
    "${CMAKE_CURRENT_SOURCE_DIR}/src/arch/${OS_ARCH_TARGET}/linker.ld"
)

# Gather all kernel sources
file(GLOB KERNEL_SOURCES
    "${CMAKE_SOURCE_DIR}/src/*.c"
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.asm"
    "${CMAKE_SOURCE_DIR}/src/Group_14/src/*.c"
    "${CMAKE_SOURCE_DIR}/src/Group_14/src/*.asm"
    "${CMAKE_SOURCE_DIR}/src/library/stdio_impl.c"
    "${CMAKE_SOURCE_DIR}/src/Group_14/src/paging_asm.asm"
)

# Build the kernel
add_executable(uiaos-kernel ${KERNEL_SOURCES})

# Include directories for the kernel target
target_include_directories(uiaos-kernel PUBLIC
    "${CMAKE_SOURCE_DIR}/include"
)

# Compiler flags for the kernel
target_compile_options(uiaos-kernel PRIVATE
    $<$<COMPILE_LANGUAGE:C>:
        -Wall -Wextra -nostdinc -nostdlib -fno-builtin -fno-stack-protector
        -fno-stack-check -fno-lto -fPIE -m32 -march=i386 -mno-mmx
        -mno-sse -mno-sse2 -mno-red-zone -Wno-main -g
    >
    $<$<COMPILE_LANGUAGE:CXX>:
        -Wall -Wextra -nostdinc -nostdlib -fno-builtin -fno-stack-protector
        -fno-stack-check -fno-lto -fPIE -m32 -march=i386 -mno-mmx
        -mno-sse -mno-sse2 -mno-red-zone -g
    >
    $<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:
        -m32 -march=i386 -Wno-unused-variable -Wno-unused-parameter
    >
)

# Linker flags for the kernel
target_link_options(uiaos-kernel PUBLIC
    $<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:
        -ffreestanding -nostdlib -fno-builtin -static -pie -O0
        -T${OS_KERNEL_LINKER} -g
    >
)

target_link_options(uiaos-kernel
    PUBLIC "-ffreestanding"
    PUBLIC "-nostdlib"
    PUBLIC "-static"
    PUBLIC "-pie"
    PUBLIC "-T${OS_KERNEL_LINKER}"
)

set_target_properties(uiaos-kernel PROPERTIES
    OUTPUT_NAME "${OS_KERNEL_BINARY}"
)

########################################
# Build a userland program: hello.elf
########################################
# Suppose you keep hello.c in "Userland/hello.c"
# and a user.ld in the PROJECT ROOT: ${CMAKE_SOURCE_DIR}/user.ld
set(USER_LD "${CMAKE_SOURCE_DIR}/user.ld")
add_executable(hello_elf "${CMAKE_SOURCE_DIR}/User/hello/hello.c")

# Example compile/link flags for i386 user program
target_compile_options(hello_elf PRIVATE
    -m32 -O2 -Wall -Wextra -fno-builtin -fno-stack-protector -fno-stack-check -fno-lto
)
target_link_options(hello_elf PRIVATE
    -m32 -nostdlib -T ${USER_LD} -static
)

set_target_properties(hello_elf PROPERTIES
    OUTPUT_NAME "hello.elf"
)

########################################
# Create an Empty FAT32 Disk Image (32MB)
########################################
set(DISK_IMAGE "${CMAKE_CURRENT_BINARY_DIR}/disk.iso")

add_custom_target(create-fat32-disk
    COMMAND dd if=/dev/zero of=${DISK_IMAGE} bs=1M count=32
    COMMAND mkfs.fat -F 32 ${DISK_IMAGE}
    COMMENT "Creating 32MB FAT32 disk image: ${DISK_IMAGE}"
    VERBATIM
)

########################################
# Copy hello.elf onto the FAT32 disk image
# (Requires 'mtools' -> 'mcopy')
########################################
add_custom_target(copy-hello ALL
    DEPENDS hello_elf create-fat32-disk
    COMMAND mcopy -i ${DISK_IMAGE} $<TARGET_FILE:hello_elf> ::/hello.elf
    COMMENT "Copying hello.elf to FAT32 disk image."
)

########################################
# OS-Image (Limine-based ISO) Target
########################################
set(ISO_DIR "${CMAKE_CURRENT_BINARY_DIR}/iso")
# If limine.cfg is located in "Group_14/limine.cfg", set it here:
set(LIMINE_CONFIG_DIR "${CMAKE_SOURCE_DIR}/Group_14")
set(LIMINE_DIR "/usr/local/limine")

add_custom_target(uiaos-create-image
    COMMAND rm -rf ${ISO_DIR}
    COMMAND mkdir -p ${ISO_DIR}
    COMMAND cp -v $<TARGET_FILE:uiaos-kernel>
        ${LIMINE_CONFIG_DIR}/limine.cfg
        ${LIMINE_DIR}/limine-bios.sys
        ${LIMINE_DIR}/limine-bios-cd.bin
        ${LIMINE_DIR}/limine-uefi-cd.bin
        ${ISO_DIR}/
    COMMAND mkdir -p ${ISO_DIR}/EFI/BOOT
    COMMAND cp -v
        ${LIMINE_DIR}/BOOTX64.EFI
        ${ISO_DIR}/EFI/BOOT/
    COMMAND cp -v
        ${LIMINE_DIR}/BOOTIA32.EFI
        ${ISO_DIR}/EFI/BOOT/

    COMMAND xorriso -as mkisofs -b limine-bios-cd.bin
        -no-emul-boot -boot-load-size 4 -boot-info-table
        --efi-boot limine-uefi-cd.bin
        -efi-boot-part --efi-boot-image --protective-msdos-label
        ${ISO_DIR} -o ${CMAKE_CURRENT_BINARY_DIR}/${OS_KERNEL_IMAGE}

    COMMAND ${LIMINE_DIR}/limine bios-install
        ${CMAKE_CURRENT_BINARY_DIR}/${OS_KERNEL_IMAGE}

    DEPENDS copy-hello uiaos-kernel
    COMMENT "Creating Limine ISO image: ${OS_KERNEL_IMAGE}"
    VERBATIM
)
