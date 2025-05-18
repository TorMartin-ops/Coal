# CMakeLists.txt - Improved FAT16 disk integration

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
list(FILTER KERNEL_SOURCES EXCLUDE REGEX ".*/shell\\.c$")
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
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -nostdinc -nostdlib -fno-builtin -fno-stack-protector -fno-stack-check -fno-lto -fPIE -m32 -march=i386 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -Wno-main -g -D KLOG_LEVEL_DEBUG> # <-- Added here
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -nostdinc -nostdlib -fno-builtin -fno-stack-protector -fno-stack-check -fno-lto -fPIE -m32 -march=i386 -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -g -D KLOG_LEVEL_DEBUG> # <-- Added here
    $<$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>:-m32 -march=i386 -Wno-unused-variable -Wno-unused-parameter -g>
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


set(OS_SHELL_ELF_BINARY "shell.elf")

add_executable(shell_elf
    shell.c     # Assuming shell.c is in the same directory as CMakeLists.txt
    entry.asm   # Reusing the same entry.asm
)

# Link Options for shell_elf using user.ld (same as hello_elf)
target_link_options(shell_elf PUBLIC
    -m32
    -nostdlib
    -static
    -T${OS_USER_LINKER}
    -g
    -lgcc
)

# Compile Options for shell_elf (same as hello_elf)
target_compile_options(shell_elf PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-m32 -Wall -Wextra -nostdlib -fno-builtin -fno-stack-protector -g>
)

set_target_properties(shell_elf PROPERTIES
    OUTPUT_NAME "${OS_SHELL_ELF_BINARY}"
)
#endregion_tag_shell_target

########################################
# Create FAT16 Disk Image and Include in ISO
########################################
set(DISK_IMAGE "${CMAKE_CURRENT_BINARY_DIR}/disk.img")

# Create a combined target for disk image creation and file copying
add_custom_command(
    OUTPUT ${DISK_IMAGE}
    # Create the FAT16 disk image
    COMMAND dd if=/dev/zero of=${DISK_IMAGE} bs=1M count=32
    COMMAND mkfs.fat ${DISK_IMAGE}
    # Copy hello.elf to the FAT16 image
    COMMAND mcopy -i ${DISK_IMAGE} $<TARGET_FILE:hello_elf> ::/hello.elf
    #region_tag_copy_shell
    # Create /bin directory and copy shell.elf to it
    COMMAND mmd -i ${DISK_IMAGE} ::/bin
    COMMAND mcopy -i ${DISK_IMAGE} $<TARGET_FILE:shell_elf> ::/bin/shell.elf
    #endregion_tag_copy_shell
    DEPENDS hello_elf shell_elf # Add shell_elf as a dependency
    COMMENT "Creating FAT disk image with hello.elf and shell.elf"
    VERBATIM
    OUTPUT ${CMAKE_BINARY_DIR}/asm/irq_stubs.o
    COMMAND nasm ${ASM_FLAGS} ${CMAKE_SOURCE_DIR}/src/irq_stubs.asm -o ${CMAKE_BINARY_DIR}/asm/irq_stubs.o -felf32 -DDEBUG_IRQ_STUBS=1 -DDEBUG_IRQ1_DIRECT=1
    DEPENDS ${CMAKE_SOURCE_DIR}/src/irq_stubs.asm
    COMMENT "[NASM] Compiling ASM object (IRQ Stubs): irq_stubs.o"
)

########################################
# OS-Image Target
########################################
set(ISO_DIR ${CMAKE_CURRENT_BINARY_DIR}/iso)
set(LIMINE_CONFIG_DIR ${CMAKE_SOURCE_DIR}) # Assumes limine.cfg is in the same dir as CMakeLists.txt
set(LIMINE_DIR /usr/local/limine) # Assuming this path is correct on your system

# Create the ISO image (depends on kernel and the FAT16 disk image)
add_custom_target(
    uiaos-create-image ALL # Build this target by default

    # Setup ISO directory
    COMMAND ${CMAKE_COMMAND} -E rm -rf ${ISO_DIR}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ISO_DIR}
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:uiaos-kernel> ${ISO_DIR}/
    # Copy the FAT16 disk image into the ISO directory
    COMMAND ${CMAKE_COMMAND} -E copy ${DISK_IMAGE} ${ISO_DIR}/
    COMMAND ${CMAKE_COMMAND} -E copy
        ${LIMINE_CONFIG_DIR}/limine.cfg
        ${LIMINE_DIR}/limine-bios.sys
        ${LIMINE_DIR}/limine-bios-cd.bin
        ${LIMINE_DIR}/limine-uefi-cd.bin
        ${ISO_DIR}/
    COMMAND ${CMAKE_COMMAND} -E make_directory ${ISO_DIR}/EFI/BOOT
    COMMAND ${CMAKE_COMMAND} -E copy ${LIMINE_DIR}/BOOTX64.EFI ${ISO_DIR}/EFI/BOOT/
    COMMAND ${CMAKE_COMMAND} -E copy ${LIMINE_DIR}/BOOTIA32.EFI ${ISO_DIR}/EFI/BOOT/

    # Create ISO command - include the FAT16 disk image
    COMMAND xorriso -as mkisofs -b limine-bios-cd.bin
        -no-emul-boot -boot-load-size 4 -boot-info-table
        --efi-boot limine-uefi-cd.bin
        -efi-boot-part --efi-boot-image --protective-msdos-label
        ${ISO_DIR} -o ${CMAKE_CURRENT_BINARY_DIR}/${OS_KERNEL_IMAGE}

    # Install bootloader command
    COMMAND ${LIMINE_DIR}/limine bios-install ${CMAKE_CURRENT_BINARY_DIR}/${OS_KERNEL_IMAGE}

    # Dependencies - note that we now depend directly on the disk image
    DEPENDS uiaos-kernel ${DISK_IMAGE}
    COMMENT "Creating Limine ISO image: ${OS_KERNEL_IMAGE} with embedded disk image"
    VERBATIM
    USES_TERMINAL
)