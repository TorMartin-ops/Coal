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
include(FetchContent) # Not used yet

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

# Command to compile NASM files (As per your working example)
set(CMAKE_ASM_NASM_COMPILE_OBJECT
    "<CMAKE_ASM_NASM_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -f ${CMAKE_ASM_NASM_OBJECT_FORMAT} -o <OBJECT> <SOURCE>")

########################################
# OS Target
########################################
set(OS_KERNEL_LINKER "${CMAKE_CURRENT_SOURCE_DIR}/src/arch/${OS_ARCH_TARGET}/linker.ld")
# --- Ensure the linker script exists at the path above, or adjust the path ---

# Create a variable containing all source files in the src directory (As per your working example)
file(GLOB SOURCES
    "src/*.c"   "src/*.cpp"   "src/*.asm"
    "src/Group_14/src/*.c" # Note: This nested path might be incorrect
    "src/Group_14/src/*.asm" # Note: This nested path might be incorrect
    "src/library/stdio_impl.c"
    "src/Group_14/src/paging_asm.asm" # Note: This nested path might be incorrect
)
# --- Verify these GLOB patterns match your source file locations ---

# Add the executable using the gathered source files
add_executable(uiaos-kernel ${SOURCES})

# Include directories for the kernel target
target_include_directories(uiaos-kernel PUBLIC
     "${CMAKE_SOURCE_DIR}/include" # Use absolute path
     # Add include/libc path if needed by sources:
     # "${CMAKE_SOURCE_DIR}/include/libc"
)


# Specify compile options for C and C++ (No -Werror)
target_compile_options(uiaos-kernel PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -nostdinc -nostdlib -fno-builtin -fno-stack-protector -fno-stack-check -fno-lto -fPIE -m32 -march=i386 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -Wno-main -g>
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -nostdinc -nostdlib -fno-builtin -fno-stack-protector -fno-stack-check -fno-lto -fPIE -m32 -march=i386 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -g>
    $<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:-m32 -march=i386 -Wno-unused-variable -Wno-unused-parameter -g> # Added -g here
)


# Specify link options for C and C++ (Using structure from your example)
target_link_options(uiaos-kernel PUBLIC
    $<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:-ffreestanding -nostdlib -fno-builtin -static -pie -O0 -T${OS_KERNEL_LINKER} -g -lgcc> # Added -lgcc
)

# Duplicate link options section (As per your working example - though redundant)
target_link_options(
    uiaos-kernel
    PUBLIC "-ffreestanding"
    PUBLIC "-nostdlib"
    PUBLIC "-static"
    PUBLIC "-pie"
    PUBLIC "-T${OS_KERNEL_LINKER}"
    PUBLIC "-g" # Added -g
    PUBLIC "-lgcc" # Added -lgcc
)

# Set properties for the kernel target
set_target_properties(uiaos-kernel PROPERTIES
    OUTPUT_NAME "${OS_KERNEL_BINARY}"
)

########################################
# Create Empty Fat32 Disk Image
########################################
set(DISK_IMAGE "${CMAKE_CURRENT_BINARY_DIR}/disk.iso")

# Custom target to create an empty FAT32 disk image of 32MB
add_custom_target(
    create-fat32-disk
    COMMAND dd if=/dev/zero of=${DISK_IMAGE} bs=1M count=32
    COMMAND mkfs.fat -F 32 ${DISK_IMAGE}
    COMMENT "Creating 32MB FAT32 disk image: ${DISK_IMAGE}" # Added comment
    VERBATIM
)

########################################
# OS-Image Target
########################################
set(ISO_DIR ${CMAKE_CURRENT_BINARY_DIR}/iso)
set(LIMINE_CONFIG_DIR ${CMAKE_SOURCE_DIR}) # Assumes limine.cfg is in Group_14 root
set(LIMINE_DIR /usr/local/limine) # Keep hardcoded path as per your example

add_custom_target(
    uiaos-create-image ALL # Add ALL to make this build by default
    COMMAND rm -rf ${ISO_DIR}
    COMMAND mkdir -p ${ISO_DIR}
    # Use cp -v as per your example
    COMMAND cp -v $<TARGET_FILE:uiaos-kernel> ${ISO_DIR}/
    COMMAND cp -v ${LIMINE_CONFIG_DIR}/limine.cfg ${LIMINE_DIR}/limine-bios.sys ${LIMINE_DIR}/limine-bios-cd.bin ${LIMINE_DIR}/limine-uefi-cd.bin ${ISO_DIR}/
    COMMAND mkdir -p ${ISO_DIR}/EFI/BOOT
    COMMAND cp -v ${LIMINE_DIR}/BOOTX64.EFI ${ISO_DIR}/EFI/BOOT/
    COMMAND cp -v ${LIMINE_DIR}/BOOTIA32.EFI ${ISO_DIR}/EFI/BOOT/
    # Note: This version does NOT copy the disk.iso into the ISO structure

    # Create ISO (Assumes xorriso is in PATH)
    COMMAND xorriso -as mkisofs -b limine-bios-cd.bin
        -no-emul-boot -boot-load-size 4 -boot-info-table
        --efi-boot limine-uefi-cd.bin
        -efi-boot-part --efi-boot-image --protective-msdos-label
        ${ISO_DIR} -o ${CMAKE_CURRENT_BINARY_DIR}/${OS_KERNEL_IMAGE} # Use variable

    # Install bootloader (Assumes limine executable is in LIMINE_DIR)
    COMMAND ${LIMINE_DIR}/limine bios-install ${CMAKE_CURRENT_BINARY_DIR}/${OS_KERNEL_IMAGE} # Use variable

    # Dependencies (No hello_elf/copy-hello in this version)
    DEPENDS create-fat32-disk uiaos-kernel
    COMMENT "Creating Limine ISO image: ${OS_KERNEL_IMAGE}" # Added comment
    VERBATIM
    USES_TERMINAL
)