# Remove the Anti that tools/install.ps1 put under $HOME\.anti.
#
#   irm https://anti-lang.com/uninstall.ps1 | iex
#   $env:ANTI_ARCH = "x86_64"; irm https://anti-lang.com/uninstall.ps1 | iex
#
# It deletes that one directory and nothing else. The installer never
# edited the PATH of the user or of the machine, so nothing else has to be
# undone. A PATH entry added by hand is named here rather than removed.
#
# ANTI_ARCH removes the install of another processor, which stands in
# $HOME\.anti-<cpu>. A copy of the file on disk takes --arm or --intel on
# its command line instead, because iex passes no arguments.
#
#   ANTI_ARCH       arm64 or x86_64, default the processor of this machine
#   ANTI_HOME       what to remove, default $HOME\.anti
#   ANTI_REMOVE     yes or no to removing the directory
$ErrorActionPreference = "Stop"

$remove = $env:ANTI_REMOVE

# The processor of the install. A file on disk takes --arm or --intel, and
# a run through iex takes ANTI_ARCH.
$arch = $env:ANTI_ARCH
foreach ($argument in $args) {
    switch ($argument) {
        "--arm"    { $arch = "arm64" }
        "--arm64"  { $arch = "arm64" }
        "--intel"  { $arch = "x86_64" }
        "--x86_64" { $arch = "x86_64" }
        "--x64"    { $arch = "x86_64" }
        default    { throw "anti: unknown option $argument" }
    }
}
if ($arch -eq "arm") { $arch = "arm64" }
if ($arch -eq "intel" -or $arch -eq "x64") { $arch = "x86_64" }

$cpu = $env:PROCESSOR_ARCHITECTURE
if ($env:PROCESSOR_ARCHITEW6432) { $cpu = $env:PROCESSOR_ARCHITEW6432 }
$native = if ($cpu -eq "ARM64") { "arm64" } else { "x86_64" }
$home_dir = if ($env:ANTI_HOME) {
    $env:ANTI_HOME
} elseif ($arch -and $arch -ne $native) {
    "$HOME\.anti-$arch"
} else {
    "$HOME\.anti"
}

function Say($text) { Write-Host "anti: $text" }

if (-not (Test-Path $home_dir)) {
    Say "$home_dir does not exist, so nothing is installed there"
    return
}

$bytes = (Get-ChildItem -Recurse -File $home_dir -EA 0 | Measure-Object Length -Sum).Sum
Say ("{0} holds {1:N0} MB" -f $home_dir, ($bytes / 1MB))
if ($remove -notmatch "^(y|Y|yes|Yes)$") {
    if ($remove -match "^(n|N|no|No)$") {
        Say "nothing removed"
        return
    }
    $answer = Read-Host "Remove $home_dir? [Y/n]"
    if ($answer -match "^(n|N|no|No)$") {
        Say "nothing removed"
        return
    }
}

Remove-Item -Recurse -Force $home_dir
Say "removed $home_dir"

# The installer may have added exactly this entry, and only that one is
# removed. A different entry naming Anti is the user's own and is named
# rather than touched.
$bin = "$home_dir\bin"
$path = [Environment]::GetEnvironmentVariable("PATH", "User")
if ($path) {
    $parts = $path -split ";"
    if ($parts -contains $bin) {
        $kept = $parts | Where-Object { $_ -ne $bin }
        [Environment]::SetEnvironmentVariable("PATH", ($kept -join ";"), "User")
        Say "removed $bin from the PATH of your account"
    } elseif ($path.Contains(".anti")) {
        Say "an entry of your own names Anti in the PATH of your account"
    }
}
