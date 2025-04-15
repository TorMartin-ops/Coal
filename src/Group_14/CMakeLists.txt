# CMakeLists.txt - Use mcopy + FAT16

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
# include(FetchContent) # Not used currently

########################################
# UiAOS: Variables
########################################
set(OS_ARCH_TARGET "i386")  # or x86_64
set(OS_NAME "UiA Operating System")
set(OS_KERNEL_NAME "uiaos")
set(OS_KERNEL_BINARY "kernel.bin")
set(OS_KERNEL_IMAGE "kernel.iso")
set(OS_HELLO_ELF_BINARY "hello.elf") # Define output name for hello

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
    "<CMAKE_ASM_NASM_COMPILER> <DEFINES> <INCLUDES> <FLAGS> -f ${CMAKE_ASM_NASM_OBJECT_FORMAT} -g -o <OBJECT> <SOURCE>")

########################################
# OS Target (Kernel)
########################################
set(OS_KERNEL_LINKER "${CMAKE_CURRENT_SOURCE_DIR}/src/arch/${OS_ARCH_TARGET}/linker.ld") # Make sure this linker script exists and is correct

# Use GLOB_RECURSE to find all source files in src directory relative to CMakeLists.txt
file(GLOB_RECURSE KERNEL_SOURCES
    "src/*.c"
    "src/*.cpp"
    "src/*.asm"
)

# Filter out user-space sources (using relative paths from CMakeLists.txt)
list(FILTER KERNEL_SOURCES EXCLUDE REGEX ".*/hello\\.c$")
list(FILTER KERNEL_SOURCES EXCLUDE REGEX ".*/entry\\.asm$")
list(FILTER KERNEL_SOURCES EXCLUDE REGEX ".*/user\\.ld$")

# Add the executable using the gathered source files
add_executable(uiaos-kernel ${KERNEL_SOURCES})

# Include directories for the kernel target
target_include_directories(uiaos-kernel PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include" # Relative path from CMakeLists.txt
)

# Specify compile options for C and C++ (Kernel)
target_compile_options(uiaos-kernel PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -nostdinc -nostdlib -fno-builtin -fno-stack-protector -fno-stack-check -fno-lto -fPIE -m32 -march=i386 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -Wno-main -g>
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -nostdinc -nostdlib -fno-builtin -fno-stack-protector -fno-stack-check -fno-lto -fPIE -m32 -march=i386 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -g>
    $<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:-m32 -march=i386 -Wno-unused-variable -Wno-unused-parameter -g> # Added -g here
)

# Specify link options for C and C++ (Kernel) - Simplified, removed redundancy
target_link_options(uiaos-kernel PUBLIC
    -ffreestanding -nostdlib -fno-builtin -static -pie -O0 -T${OS_KERNEL_LINKER} -g -lgcc # Added -lgcc
)

# Set properties for the kernel target
set_target_properties(uiaos-kernel PROPERTIES
    OUTPUT_NAME "${OS_KERNEL_BINARY}"
)

########################################
# User Space Program Target (hello.elf)
########################################
# Use correct relative paths from CMakeLists.txt
add_executable(hello_elf
    hello.c     # Assuming hello.c is in the same directory as CMakeLists.txt
    entry.asm   # Assuming entry.asm is in the same directory as CMakeLists.txt
)

set(OS_USER_LINKER "${CMAKE_CURRENT_SOURCE_DIR}/src/arch/${OS_ARCH_TARGET}/user.ld") # Path relative to CMakeLists.txt

# Link Options for hello_elf using user.ld
target_link_options(hello_elf PUBLIC
    -m32
    -nostdlib
    -static
    -T${OS_USER_LINKER} # <-- Use the user linker script!
    -g
    -lgcc
)

# Compile Options for hello_elf
target_compile_options(hello_elf PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-m32 -Wall -Wextra -nostdlib -fno-builtin -fno-stack-protector -g>
)

set_target_properties(hello_elf PROPERTIES
    OUTPUT_NAME "${OS_HELLO_ELF_BINARY}"
)

########################################
# Create Empty Fat16 Disk Image
########################################
set(DISK_IMAGE "${CMAKE_CURRENT_BINARY_DIR}/disk.iso")

# Custom target to create an empty FAT16 disk image of 32MB
add_custom_target(
    create-fat16-disk # Renamed target slightly for clarity
    COMMAND dd if=/dev/zero of=${DISK_IMAGE} bs=1M count=32
    # --- Changed: Use -F 16 for FAT16 ---
    COMMAND mkfs.fat -F 16 ${DISK_IMAGE}
    COMMENT "Creating 32MB FAT16 disk image: ${DISK_IMAGE}" # Changed comment
    VERBATIM
)

########################################
# OS-Image Target
########################################
set(ISO_DIR ${CMAKE_CURRENT_BINARY_DIR}/iso)
set(LIMINE_CONFIG_DIR ${CMAKE_SOURCE_DIR}) # Assumes limine.cfg is in the same dir as CMakeLists.txt
set(LIMINE_DIR /usr/local/limine) # Assuming this path is correct on your system

# Step 1: Copy hello.elf to the disk image using mcopy
add_custom_command(
    OUTPUT ${DISK_IMAGE}.stamp # A dummy file to track completion
    # --- Changed: Reverted back to mcopy ---
    COMMAND mcopy -i ${DISK_IMAGE} $<TARGET_FILE:hello_elf> ::/hello.elf
    # --- Make sure mtools package is installed ---
    DEPENDS create-fat16-disk hello_elf # Depends on FAT16 disk creation and hello_elf build
    COMMENT "Copying hello.elf to disk image (using mcopy)..."
    VERBATIM
)

# Step 2: Create the ISO image (depends on kernel and the copy step)
add_custom_target(
    uiaos-create-image ALL # Build this target by default

    # Setup ISO directory
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${ISO_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ISO_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:uiaos-kernel> ${ISO_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy
        ${LIMINE_CONFIG_DIR}/limine.cfg
        ${LIMINE_DIR}/limine-bios.sys
        ${LIMINE_DIR}/limine-bios-cd.bin
        ${LIMINE_DIR}/limine-uefi-cd.bin
        ${ISO_DIR}/
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ISO_DIR}/EFI/BOOT
    COMMAND ${CMAKE_COMMAND} -E copy ${LIMINE_DIR}/BOOTX64.EFI ${ISO_DIR}/EFI/BOOT/
    COMMAND ${CMAKE_COMMAND} -E copy ${LIMINE_DIR}/BOOTIA32.EFI ${ISO_DIR}/EFI/BOOT/

    # Create ISO command
    COMMAND xorriso -as mkisofs -b limine-bios-cd.bin
        -no-emul-boot -boot-load-size 4 -boot-info-table
        --efi-boot limine-uefi-cd.bin
        -efi-boot-part --efi-boot-image --protective-msdos-label
        ${ISO_DIR} -o ${CMAKE_CURRENT_BINARY_DIR}/${OS_KERNEL_IMAGE}

    # Install bootloader command
    COMMAND ${LIMINE_DIR}/limine bios-install ${CMAKE_CURRENT_BINARY_DIR}/${OS_KERNEL_IMAGE}

    # Dependencies
    DEPENDS uiaos-kernel ${DISK_IMAGE}.stamp # Depends on kernel and the stamp file from the copy command
    COMMENT "Creating Limine ISO image: ${OS_KERNEL_IMAGE} and populating disk"
    VERBATIM
    USES_TERMINAL
)