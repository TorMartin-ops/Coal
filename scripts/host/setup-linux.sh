#!/bin/sh
# Checks a Linux host for running Coal OS under QEMU with KVM, and installs QEMU when asked.
# Usage: setup-linux.sh [-y]     -y answers yes to the install question.
# Safe to run again; it changes nothing that is already right.
set -u

ASSUME_YES=0
[ "${1:-}" = "-y" ] && ASSUME_YES=1

PASS=0
RESULTS=""

report() {   # status name detail
    RESULTS="$RESULTS$(printf '%-6s %-24s %s' "$1" "$2" "$3")
"
    [ "$1" = "FAIL" ] && PASS=1
}

ask() {      # question; true when answered yes
    [ "$ASSUME_YES" = 1 ] && return 0
    [ -t 0 ] || return 1
    printf '%s [y/N] ' "$1"
    read -r answer
    case "$answer" in y | Y | yes | YES) return 0 ;; *) return 1 ;; esac
}

as_root() {
    if [ "$(id -u)" = 0 ]; then "$@"; elif command -v sudo >/dev/null 2>&1; then sudo "$@"; else return 1; fi
}

install_qemu() {
    if command -v apt-get >/dev/null 2>&1; then
        as_root apt-get update -qq && as_root apt-get install -y qemu-system-x86
    elif command -v dnf >/dev/null 2>&1; then
        as_root dnf install -y qemu-system-x86
    elif command -v pacman >/dev/null 2>&1; then
        as_root pacman -S --noconfirm qemu-system-x86
    elif command -v zypper >/dev/null 2>&1; then
        as_root zypper install -y qemu-x86
    else
        echo "No supported package manager found; install QEMU (system emulation, x86) by hand." >&2
        return 1
    fi
}

# --- CPU and /dev/kvm ---------------------------------------------------------------------------

if grep -q -E 'vmx|svm' /proc/cpuinfo 2>/dev/null; then
    report PASS "CPU virtualization" "vmx/svm flag present"
else
    report FAIL "CPU virtualization" "no vmx/svm flag; enable VT-x/AMD-V in the firmware, or this is a VM without nested virtualization"
fi

if [ -e /dev/kvm ]; then
    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
        report PASS "/dev/kvm" "$(ls -l /dev/kvm)"
    else
        group=$(stat -c %G /dev/kvm 2>/dev/null || echo kvm)
        if id -nG | tr ' ' '\n' | grep -qx "$group"; then
            report FAIL "/dev/kvm" "not accessible although you are in group $group; log in again"
        else
            report FAIL "/dev/kvm" "not accessible; run: sudo usermod -aG $group $(id -un)   (then log in again)"
        fi
    fi
else
    report FAIL "/dev/kvm" "missing; load the module: sudo modprobe kvm_intel (or kvm_amd), and check the firmware setting"
fi

if command -v kvm-ok >/dev/null 2>&1; then
    report INFO "kvm-ok" "$(kvm-ok 2>&1 | tr '\n' ' ')"
fi

# --- QEMU ---------------------------------------------------------------------------------------

missing_qemu() {
    ! command -v qemu-system-x86_64 >/dev/null 2>&1
}

if missing_qemu && ask "Install QEMU (qemu-system-x86) now?"; then
    install_qemu || true
fi

if missing_qemu; then
    report FAIL "QEMU" "qemu-system-x86_64 not found; install qemu-system-x86 (apt, dnf, pacman) or run with -y"
else
    report PASS "QEMU" "$(qemu-system-x86_64 --version | head -n 1)"
fi

# --- Tools the build needs ----------------------------------------------------------------------

for tool in cmake nasm gcc grub-mkrescue mformat python3; do
    if command -v "$tool" >/dev/null 2>&1; then
        report PASS "tool: $tool" "$(command -v "$tool")"
    else
        report FAIL "tool: $tool" "not found; needed to build and drive Coal OS (see the top-level README)"
    fi
done

printf '%s' "$RESULTS"
echo
if [ "$PASS" = 0 ]; then
    echo "Ready: run QEMU with -accel kvm."
else
    echo "Fix the FAIL lines above and run this script again. Without KVM QEMU still works (TCG), only slower."
fi
exit "$PASS"
