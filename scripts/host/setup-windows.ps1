<#
.SYNOPSIS
    Prepares a Windows PC to run Coal OS under QEMU with hardware acceleration.

.DESCRIPTION
    Checks the Windows build, CPU virtualization, Hyper-V features and WSL2, enables nested
    virtualization in %UserProfile%\.wslconfig (other keys are kept, a backup is written first),
    checks /dev/kvm and QEMU inside the default WSL distribution, and tests QEMU with WHPX on
    Windows as the fallback. Prints a PASS/FAIL table and the recommended path.

    Run it from an elevated PowerShell (Windows PowerShell 5.1 or newer):
        powershell -ExecutionPolicy Bypass -File scripts\host\setup-windows.ps1

    Safe to run again: it changes nothing that is already right, and asks before it restarts
    WSL or installs packages.

.PARAMETER Distro
    WSL distribution to check. Default: the default distribution.

.PARAMETER Memory
    Memory for the WSL2 VM, for example 12GB. Default: half of the RAM (4GB to 16GB) when
    .wslconfig has no memory setting; an existing setting is left alone unless you pass this.

.PARAMETER Yes
    Answer yes to the questions (restart WSL, install qemu-system-x86).
#>
[CmdletBinding()]
param(
    [string]$Distro,
    [string]$Memory,
    [switch]$Yes
)

$ErrorActionPreference = 'Continue'
$script:Results = New-Object System.Collections.ArrayList

function Add-Result {
    param([string]$Name, [string]$Status, [string]$Detail)
    [void]$script:Results.Add([pscustomobject]@{ Name = $Name; Status = $Status; Detail = $Detail })
}

function Write-Step {
    param([string]$Text)
    Write-Host ""
    Write-Host "== $Text" -ForegroundColor Cyan
}

function Confirm-Action {
    param([string]$Question)
    if ($Yes) { return $true }
    try { $answer = Read-Host "$Question [y/N]" } catch { return $false }
    return ($answer -match '^(y|yes)$')
}

# wsl.exe prints UTF-16; strip the NUL bytes PowerShell 5.1 leaves in the text.
function Invoke-Wsl {
    param([string[]]$WslArgs)
    $text = $null
    $code = -1
    try {
        $raw = & wsl.exe @WslArgs 2>&1
        $code = $LASTEXITCODE
        $text = (($raw | ForEach-Object { "$_" }) -join "`n") -replace "`0", ""
    } catch {
        $text = $_.Exception.Message
    }
    return [pscustomobject]@{ ExitCode = $code; Text = ([string]$text).Trim() }
}

# --- .wslconfig editing: line based, so comments, ordering and unrelated keys survive -----------

function Get-IniValue {
    param([System.Collections.Generic.List[string]]$Lines, [string]$Section, [string]$Key)
    $inSection = $false
    foreach ($line in $Lines) {
        if ($line -match '^\s*\[(.+?)\]\s*$') { $inSection = ($Matches[1].Trim() -ieq $Section); continue }
        if ($inSection -and $line -match ('^\s*' + [regex]::Escape($Key) + '\s*=\s*(.*?)\s*$')) { return $Matches[1] }
    }
    return $null
}

function Set-IniValue {
    param([System.Collections.Generic.List[string]]$Lines, [string]$Section, [string]$Key, [string]$Value)
    $start = -1
    $end = $Lines.Count
    for ($i = 0; $i -lt $Lines.Count; $i++) {
        if ($Lines[$i] -match '^\s*\[(.+?)\]\s*$') {
            if ($start -ge 0) { $end = $i; break }
            if ($Matches[1].Trim() -ieq $Section) { $start = $i }
        }
    }
    if ($start -lt 0) {
        if ($Lines.Count -gt 0 -and $Lines[$Lines.Count - 1].Trim() -ne '') { $Lines.Add('') }
        $Lines.Add("[$Section]")
        $Lines.Add("$Key=$Value")
        return
    }
    for ($i = $start + 1; $i -lt $end; $i++) {
        if ($Lines[$i] -match ('^\s*' + [regex]::Escape($Key) + '\s*=')) { $Lines[$i] = "$Key=$Value"; return }
    }
    $at = $end
    while ($at -gt $start + 1 -and $Lines[$at - 1].Trim() -eq '') { $at-- }
    $Lines.Insert($at, "$Key=$Value")
}

function Update-WslConfig {
    param([string]$Path, [hashtable]$Wanted, [string]$Override)
    $lines = New-Object 'System.Collections.Generic.List[string]'
    $existed = Test-Path -LiteralPath $Path
    if ($existed) { foreach ($l in [System.IO.File]::ReadAllLines($Path)) { $lines.Add($l) } }
    $changed = @()
    foreach ($key in $Wanted.Keys) {
        $now = Get-IniValue -Lines $lines -Section 'wsl2' -Key $key
        $want = $Wanted[$key]
        $keepExisting = ($key -eq 'memory' -and $now -and -not $Override)
        if ($keepExisting -or ($now -and $now -ieq $want)) { continue }
        Set-IniValue -Lines $lines -Section 'wsl2' -Key $key -Value $want
        $changed += "$key=$want"
    }
    if ($changed.Count -eq 0) { return $changed }
    if ($existed) {
        $backup = "$Path.bak-" + (Get-Date -Format 'yyyyMMdd-HHmmss')
        Copy-Item -LiteralPath $Path -Destination $backup
        Write-Host "Backed up .wslconfig to $backup"
    }
    [System.IO.File]::WriteAllLines($Path, $lines.ToArray(), (New-Object System.Text.UTF8Encoding($false)))
    return $changed
}

# --- 1. Windows ---------------------------------------------------------------------------------

Write-Step 'Windows'
$os = Get-CimInstance Win32_OperatingSystem
$edition = (Get-ItemProperty 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -ErrorAction SilentlyContinue).EditionID
$build = [int]$os.BuildNumber
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Host ("{0}, build {1}, edition {2}" -f $os.Caption, $build, $edition)
Add-Result 'Windows build' 'INFO' ("{0} build {1} ({2})" -f $os.Caption, $build, $edition)
if ($build -ge 22000) {
    Add-Result 'Windows 11 (nested WSL2)' 'PASS' 'nested virtualization in WSL2 is supported'
} elseif ($build -ge 19041) {
    Add-Result 'Windows 11 (nested WSL2)' 'WARN' 'Windows 10: WSL2 works, KVM inside it is not supported; use QEMU+WHPX'
} else {
    Add-Result 'Windows 11 (nested WSL2)' 'FAIL' 'build is too old for WSL2 (needs 19041 or newer)'
}
if (-not $isAdmin) {
    Add-Result 'Administrator' 'WARN' 'not elevated; feature checks and fixes may be incomplete'
}

# --- 2. Virtualization --------------------------------------------------------------------------

Write-Step 'Virtualization'
$cpu = Get-CimInstance Win32_Processor | Select-Object -First 1
$sys = Get-CimInstance Win32_ComputerSystem
$firmware = [bool]$cpu.VirtualizationFirmwareEnabled
$hypervisor = [bool]$sys.HypervisorPresent
Write-Host ("CPU: {0}" -f $cpu.Name.Trim())
# With a hypervisor already running (Hyper-V, WSL2) the firmware flag can read False.
if ($firmware -or $hypervisor) {
    Add-Result 'CPU virtualization' 'PASS' ("firmware enabled: {0}, hypervisor running: {1}" -f $firmware, $hypervisor)
} else {
    Add-Result 'CPU virtualization' 'FAIL' 'disabled; enable Intel VT-x / AMD-V (SVM) in the BIOS or UEFI setup'
}

foreach ($f in @(
        @{ Name = 'Microsoft-Hyper-V-All'; Label = 'Hyper-V'; Need = $false },
        @{ Name = 'VirtualMachinePlatform'; Label = 'Virtual Machine Platform'; Need = $true },
        @{ Name = 'HypervisorPlatform'; Label = 'Windows Hypervisor Platform'; Need = $false })) {
    $state = $null
    try { $state = (Get-WindowsOptionalFeature -Online -FeatureName $f.Name -ErrorAction Stop).State } catch { }
    if ($state -eq 'Enabled') {
        Add-Result $f.Label 'PASS' 'enabled'
    } elseif ($null -eq $state) {
        Add-Result $f.Label 'INFO' 'state unknown (needs an elevated shell, or not part of this edition)'
    } elseif ($f.Need) {
        Add-Result $f.Label 'FAIL' "disabled; run: Enable-WindowsOptionalFeature -Online -FeatureName $($f.Name)"
    } else {
        Add-Result $f.Label 'INFO' "$state (only needed for QEMU+WHPX: Hyper-V or Windows Hypervisor Platform)"
    }
}

# --- 3. WSL -------------------------------------------------------------------------------------

Write-Step 'WSL'
$wslOk = $false
$wslDistro = $Distro
if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) {
    Add-Result 'WSL installed' 'FAIL' 'wsl.exe not found; run: wsl --install'
} else {
    $ver = Invoke-Wsl @('--version')
    if ($ver.ExitCode -eq 0 -and $ver.Text) {
        $lines = $ver.Text -split "`n"
        $wslVersion = ([regex]::Match($lines[0], '\d+(\.\d+)+')).Value
        $kernel = ''
        if ($lines.Count -gt 1) { $kernel = ([regex]::Match($lines[1], '\d+(\.\d+)+\S*')).Value }
        Add-Result 'WSL version' 'PASS' ("WSL {0}, kernel {1}" -f $wslVersion, $kernel)
        $wslOk = $true
    } else {
        Add-Result 'WSL version' 'WARN' 'wsl --version failed (inbox WSL?); run: wsl --update'
    }

    $list = Invoke-Wsl @('-l', '-v')
    $distros = @()
    foreach ($line in ($list.Text -split "`n")) {
        if ($line -match '^\s*(\*)?\s*(\S+)\s+(\S+)\s+(\d)\s*$') {
            $distros += [pscustomobject]@{ Name = $Matches[2]; State = $Matches[3]; Version = [int]$Matches[4]; Default = [bool]$Matches[1] }
        }
    }
    if (-not $wslDistro) {
        $default = $distros | Where-Object { $_.Default } | Select-Object -First 1
        if ($default) { $wslDistro = $default.Name }
    }
    $chosen = $distros | Where-Object { $_.Name -ieq $wslDistro } | Select-Object -First 1
    if (-not $chosen) {
        Add-Result 'WSL distribution' 'FAIL' 'none installed; run: wsl --install -d Ubuntu'
        $wslDistro = $null
    } elseif ($chosen.Version -ne 2) {
        Add-Result 'WSL distribution' 'FAIL' ("{0} is WSL{1}; run: wsl --set-version {0} 2" -f $chosen.Name, $chosen.Version)
        $wslDistro = $null
    } else {
        Add-Result 'WSL distribution' 'PASS' ("{0} (WSL2, {1})" -f $chosen.Name, $chosen.State)
        if ($chosen.Name -match '^docker-desktop') {
            Add-Result 'WSL distribution' 'WARN' 'this is a Docker helper distribution; pass -Distro Ubuntu'
        }
    }
}

# --- 4. .wslconfig ------------------------------------------------------------------------------

Write-Step '.wslconfig'
$configPath = Join-Path $env:USERPROFILE '.wslconfig'
$memoryGb = [int][Math]::Floor($sys.TotalPhysicalMemory / 1GB / 2)
$memoryGb = [Math]::Max(4, [Math]::Min(16, $memoryGb))
$memoryValue = if ($Memory) { $Memory } else { "${memoryGb}GB" }
$changed = @()
try {
    $changed = @(Update-WslConfig -Path $configPath -Wanted @{ nestedVirtualization = 'true'; memory = $memoryValue } -Override $Memory)
    if ($changed.Count -gt 0) {
        Add-Result '.wslconfig' 'PASS' ("updated {0}: {1}" -f $configPath, ($changed -join ', '))
    } else {
        Add-Result '.wslconfig' 'PASS' 'nestedVirtualization=true already set; nothing to change'
    }
} catch {
    Add-Result '.wslconfig' 'FAIL' ("cannot update {0}: {1}" -f $configPath, $_.Exception.Message)
}

if ($wslOk -and $wslDistro -and $changed.Count -gt 0) {
    if (Confirm-Action 'The new .wslconfig applies after WSL restarts. Run "wsl --shutdown" now (stops all WSL distributions)?') {
        [void](Invoke-Wsl @('--shutdown'))
        Start-Sleep -Seconds 8
        Add-Result 'WSL restart' 'PASS' 'wsl --shutdown done; WSL starts again on the next use'
    } else {
        Add-Result 'WSL restart' 'WARN' 'skipped; run "wsl --shutdown" before expecting /dev/kvm'
    }
}

# --- 5. Inside WSL ------------------------------------------------------------------------------

$checkScript = @'
echo "KERNEL=$(uname -r)"
if [ -e /dev/kvm ]; then echo "KVM_NODE=$(ls -l /dev/kvm)"; else echo "KVM_NODE=missing"; fi
if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then echo "KVM_RW=yes"; else echo "KVM_RW=no"; fi
echo "CPU_VIRT=$(grep -c -E 'vmx|svm' /proc/cpuinfo)"
if command -v kvm-ok >/dev/null 2>&1; then echo "KVM_OK=$(kvm-ok 2>&1 | tr '\n' ' ')"; else echo "KVM_OK=unavailable"; fi
echo "USER_GROUPS=$(id -nG)"
for q in qemu-system-x86_64 qemu-system-i386; do
  if command -v $q >/dev/null 2>&1; then echo "QEMU_$q=yes"; else echo "QEMU_$q=missing"; fi
done
if command -v apt-get >/dev/null 2>&1; then echo "APT=yes"; else echo "APT=no"; fi
'@

# Runs a bash script in the distribution; base64 keeps quoting out of the way. Returns KEY=value pairs.
function Invoke-WslScript {
    param([string]$Script, [string]$User)
    $b64 = [Convert]::ToBase64String([Text.Encoding]::ASCII.GetBytes(($Script -replace "`r", '')))
    $wslArgs = @('-d', $wslDistro)
    if ($User) { $wslArgs += @('-u', $User) }
    $wslArgs += @('--', 'bash', '-c', "echo $b64 | base64 -d | bash")
    $result = Invoke-Wsl $wslArgs
    $pairs = @{}
    foreach ($line in ($result.Text -split "`n")) {
        if ($line -match '^([A-Za-z_0-9-]+)=(.*)$') { $pairs[$Matches[1]] = $Matches[2].Trim() }
    }
    return [pscustomobject]@{ ExitCode = $result.ExitCode; Text = $result.Text; Pairs = $pairs }
}

$kvmReady = $false
$qemuInWsl = $false
Write-Step 'KVM inside WSL'
if (-not ($wslOk -and $wslDistro)) {
    Add-Result '/dev/kvm in WSL' 'SKIP' 'WSL2 is not ready'
} else {
    $probe = Invoke-WslScript $checkScript
    if ($probe.Pairs.Count -eq 0) {
        Add-Result '/dev/kvm in WSL' 'FAIL' ("could not run bash in {0}: {1}" -f $wslDistro, $probe.Text)
    } else {
        $p = $probe.Pairs
        $needQemu = ($p['QEMU_qemu-system-x86_64'] -ne 'yes') -or ($p['QEMU_qemu-system-i386'] -ne 'yes')
        if ($needQemu -and $p['APT'] -eq 'yes') {
            if (Confirm-Action "Install qemu-system-x86 in $wslDistro (apt, runs as root in WSL)?") {
                $null = Invoke-WslScript "export DEBIAN_FRONTEND=noninteractive; apt-get update -qq && apt-get install -y -qq qemu-system-x86 cpu-checker" 'root'
                $probe = Invoke-WslScript $checkScript
                $p = $probe.Pairs
            }
        }
        Write-Host ("kernel: {0}" -f $p['KERNEL'])
        Write-Host ("/dev/kvm: {0}" -f $p['KVM_NODE'])
        Write-Host ("kvm-ok: {0}" -f $p['KVM_OK'])
        if ($p['KVM_NODE'] -ne 'missing' -and $p['KVM_RW'] -eq 'yes') {
            Add-Result '/dev/kvm in WSL' 'PASS' $p['KVM_NODE']
            $kvmReady = $true
        } elseif ($p['KVM_NODE'] -ne 'missing') {
            Add-Result '/dev/kvm in WSL' 'FAIL' 'exists but is not readable and writable; run in WSL: sudo usermod -aG kvm $USER, then wsl --shutdown'
        } else {
            Add-Result '/dev/kvm in WSL' 'FAIL' 'missing; needs Windows 11, nestedVirtualization=true in .wslconfig and "wsl --shutdown"'
        }
        if ($p['QEMU_qemu-system-x86_64'] -eq 'yes' -and $p['QEMU_qemu-system-i386'] -eq 'yes') {
            Add-Result 'QEMU in WSL' 'PASS' 'qemu-system-x86_64 and qemu-system-i386 installed'
            $qemuInWsl = $true
        } elseif ($p['APT'] -eq 'yes') {
            Add-Result 'QEMU in WSL' 'FAIL' "missing; run in WSL: sudo apt install qemu-system-x86"
        } else {
            Add-Result 'QEMU in WSL' 'FAIL' 'missing; this distribution has no apt-get, install QEMU with its package manager'
        }
    }
}

# --- 6. QEMU with WHPX on Windows (fallback) ------------------------------------------------------

Write-Step 'QEMU with WHPX on Windows'
$whpxReady = $false
$qemuExe = $null
$found = Get-Command 'qemu-system-x86_64.exe' -ErrorAction SilentlyContinue
if ($found) { $qemuExe = $found.Source }
if (-not $qemuExe) {
    foreach ($dir in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        if ($dir -and (Test-Path -LiteralPath (Join-Path $dir 'qemu\qemu-system-x86_64.exe'))) {
            $qemuExe = Join-Path $dir 'qemu\qemu-system-x86_64.exe'
            break
        }
    }
}
if (-not $qemuExe) {
    Add-Result 'QEMU for Windows' 'INFO' 'qemu-system-x86_64.exe not found (only needed as the fallback; https://qemu.weilnetz.de/w64/)'
} else {
    Add-Result 'QEMU for Windows' 'PASS' $qemuExe
    $accels = (& $qemuExe -accel help 2>&1 | Out-String)
    if ($accels -notmatch 'whpx') {
        Add-Result 'QEMU -accel whpx' 'FAIL' 'this QEMU build has no WHPX support'
    } else {
        # QEMU stays up (stopped at reset, no display) when the accelerator initialises; it exits when it cannot.
        $errFile = [IO.Path]::GetTempFileName()
        $proc = Start-Process -FilePath $qemuExe -PassThru -WindowStyle Hidden -RedirectStandardError $errFile `
            -ArgumentList @('-accel', 'whpx', '-machine', 'q35', '-m', '64', '-display', 'none', '-nodefaults', '-S')
        Start-Sleep -Seconds 4
        if ($proc.HasExited) {
            $why = (Get-Content -LiteralPath $errFile -ErrorAction SilentlyContinue | Select-Object -First 1)
            Add-Result 'QEMU -accel whpx' 'FAIL' ("QEMU exited: {0}; enable Windows Hypervisor Platform and reboot" -f $why)
        } else {
            Stop-Process -Id $proc.Id -Force
            Add-Result 'QEMU -accel whpx' 'PASS' 'accelerator initialised'
            $whpxReady = $true
        }
        Remove-Item -LiteralPath $errFile -ErrorAction SilentlyContinue
    }
}

# --- Summary ------------------------------------------------------------------------------------

Write-Step 'Summary'
$colors = @{ PASS = 'Green'; FAIL = 'Red'; WARN = 'Yellow'; INFO = 'Gray'; SKIP = 'Gray' }
foreach ($r in $script:Results) {
    Write-Host ("{0,-6} {1,-30} {2}" -f $r.Status, $r.Name, $r.Detail) -ForegroundColor $colors[$r.Status]
}

Write-Host ""
$ok = $true
if ($kvmReady -and $qemuInWsl) {
    Write-Host 'Recommended path: KVM in WSL2. Run Coal OS with QEMU inside WSL: qemu-system-i386 -accel kvm ...' -ForegroundColor Green
} elseif ($whpxReady) {
    Write-Host 'Recommended path: QEMU with WHPX on Windows (KVM in WSL2 is not ready): qemu-system-x86_64 -accel whpx ...' -ForegroundColor Yellow
} else {
    Write-Host 'No accelerated path works yet. Fix the FAIL lines above, then run this script again.' -ForegroundColor Red
    Write-Host 'Without acceleration QEMU still runs Coal OS (TCG), only slower.'
    $ok = $false
}
if ($ok) { exit 0 } else { exit 1 }
