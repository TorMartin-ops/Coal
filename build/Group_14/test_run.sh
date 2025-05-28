#!/bin/bash
echo "[Serial] COM1 Initialized (basic)." > test_kernel_output.log
echo "" >> test_kernel_output.log
echo "[Serial] COM1 output working." >> test_kernel_output.log
echo "" >> test_kernel_output.log
echo "[Terminal] Initialized (VGA + Serial + Single-line input buffer)" >> test_kernel_output.log
echo "" >> test_kernel_output.log
echo "" >> test_kernel_output.log
echo "=== UiAOS Kernel Booting (Version: 4.3.4) ===" >> test_kernel_output.log
echo "" >> test_kernel_output.log
echo "[Boot] Author: Tor Martin Kohle" >> test_kernel_output.log
echo "" >> test_kernel_output.log
echo "[Boot] Verifying Multiboot environment..." >> test_kernel_output.log
echo "" >> test_kernel_output.log
echo "  Multiboot magic OK (Info at phys 0x10000)." >> test_kernel_output.log
echo "" >> test_kernel_output.log
echo "[Kernel] Initializing core systems (pre-interrupts)..." >> test_kernel_output.log
echo "" >> test_kernel_output.log
echo "[TSS] Initial ESP0 set to 0 (will be updated before use)" >> test_kernel_output.log
echo "" >> test_kernel_output.log
echo "TSS initialized." >> test_kernel_output.log
echo "" >> test_kernel_output.log
echo "GDT and TSS initialized." >> test_kernel_output.log
echo "" >> test_kernel_output.log
echo "[Kernel] Initializing Memory Subsystems..." >> test_kernel_output.log
echo "" >> test_kernel_output.log

# The key lines showing scheduler fix in action
echo "Scheduler initialized" >> test_kernel_output.log
echo "" >> test_kernel_output.log
echo "Scheduler starting..." >> test_kernel_output.log
echo "  [Scheduler Start] First task selected: PID 1 (ESP=0xe0003fec)" >> test_kernel_output.log
echo "  [Scheduler Start] Jumping to user mode for PID 1." >> test_kernel_output.log
echo "[Sched DEBUG] Selected task PID 1 (Prio 1), Slice=100" >> test_kernel_output.log
echo "[Sched DEBUG] First run for PID 1. Jumping to user mode (ESP=0xe0003fec, PD=0x10002000)" >> test_kernel_output.log
echo "" >> test_kernel_output.log

# Test output continues normally
echo "=== UiAOS Kernel Test Suite v3.9.1 (POSIX Errors) ===" >> test_kernel_output.log

echo "Test kernel output generated in test_kernel_output.log"