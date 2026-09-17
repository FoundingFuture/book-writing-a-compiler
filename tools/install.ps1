# Install Anti for the user who runs it, under $HOME\.anti.
#
#   irm https://anti-lang.com/install.ps1 | iex
#   $env:ANTI_ARCH = "x86_64"; irm https://anti-lang.com/install.ps1 | iex
#
# It downloads the package of this host, checks its SHA-256 against the
# SHA256SUMS beside it, unpacks it and offers the sysroot of Windows. The
# C runtime and the Windows SDK belong to Microsoft, so the script asks
# before xwin fetches them. Unpacking uses tar.exe, which Windows 10 and
# later carry.
#
# ANTI_ARCH takes the package of another processor, which Windows on ARM
# runs under emulation. A copy of the file on disk takes --arm or --intel
# on its command line instead, because iex passes no arguments.
#
#   ANTI_VERSION    the version to install, default the newest
#   ANTI_ARCH       arm64 or x86_64, default the processor of this machine
#   ANTI_HOME       where to install, default $HOME\.anti, and
#                   $HOME\.anti-<cpu> for another processor
#   ANTI_REPLACE    yes or no to replacing an install that is there
#   ANTI_PATH       yes or no to the entry in the PATH of the account
$ErrorActionPreference = "Stop"

$base = "https://anti-lang.com/downloads/resources"
$version = $env:ANTI_VERSION

# The processor of the package. A file on disk takes --arm or --intel,
# and a run through iex takes ANTI_ARCH.
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

function Say($text) { Write-Host "anti: $text" }
function Fail($text) { throw "anti: $text" }

# Ask one yes or no question. The variable of its name answers it, and a
# plain return accepts.
function Ask($name, $question) {
    $given = [Environment]::GetEnvironmentVariable("ANTI_$name")
    if ($given -match "^(y|Y|yes|Yes)$") { return $true }
    if ($given -match "^(n|N|no|No)$") { return $false }
    $answer = Read-Host "$question [Y/n]"
    return $answer -notmatch "^(n|N|no|No)$"
}

# Windows PowerShell 5.1 does not load RuntimeInformation, so the
# processor comes from the environment. A 32-bit shell on a 64-bit Windows
# reports the real processor in PROCESSOR_ARCHITEW6432.
$cpu = $env:PROCESSOR_ARCHITECTURE
if ($env:PROCESSOR_ARCHITEW6432) { $cpu = $env:PROCESSOR_ARCHITEW6432 }
switch ($cpu) {
    "AMD64" { $native = "x86_64" }
    "ARM64" { $native = "arm64" }
    default { Fail "no package for the processor $cpu" }
}
if (-not $arch) { $arch = $native }
if ($arch -ne "arm64" -and $arch -ne "x86_64") {
    Fail "the processor $arch has no package, only arm64 and x86_64"
}
$host_name = "windows-$arch"

# DESIGN: a package of the other processor runs under emulation and is
# installed beside the native one, never over it. Both directories then
# work, and the one on the PATH stays the native one.
if ($arch -eq $native) {
    $home_dir = if ($env:ANTI_HOME) { $env:ANTI_HOME } else { "$HOME\.anti" }
} else {
    $home_dir = if ($env:ANTI_HOME) { $env:ANTI_HOME } else { "$HOME\.anti-$arch" }
    Say "installing the $arch package, which this machine runs under emulation"
}

if (-not $version) {
    $version = (Invoke-RestMethod "$base/anti/latest").Trim()
}
$asset = "anti-$version-$host_name.tar.xz"
$work = Join-Path ([System.IO.Path]::GetTempPath()) ([System.IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Force $work | Out-Null

try {
    Say "downloading $asset"
    Invoke-WebRequest "$base/anti/$version/$asset" -OutFile "$work\$asset"
    $sums = Invoke-RestMethod "$base/anti/$version/SHA256SUMS"
    $want = ($sums -split "`n" | Where-Object { $_ -match [regex]::Escape($asset) }) -split "\s+" | Select-Object -First 1
    $got = (Get-FileHash "$work\$asset" -Algorithm SHA256).Hash.ToLower()
    if ($want -ne $got) { Fail "$asset has SHA-256 $got, expected $want" }

    if ((Test-Path $home_dir) -and -not (Ask REPLACE "$home_dir exists. Replace it?")) {
        Fail "nothing installed"
    }
    Remove-Item -Recurse -Force $home_dir -ErrorAction SilentlyContinue
    tar.exe -xJf "$work\$asset" -C $work
    Move-Item "$work\anti" $home_dir
    Say "installed $version in $home_dir"

    # DESIGN: the site serves this installer, so it is always the
    # newest. The CMake scripts it drives come from the package, which is
    # as old as the version installed. The number rises when the options
    # it passes change, and a package older than that fails here rather
    # than halfway.
    $PACKAGE_API = 1
    $api = 0
    if (Test-Path "$home_dir\tools\package-api") {
        $api = [int]((Get-Content "$home_dir\tools\package-api" -Raw).Trim())
    }
    if ($api -lt $PACKAGE_API) {
        Fail "Anti $version speaks version $api of the installer interface, and this installer needs $PACKAGE_API. Install a newer version of Anti."
    }

    # lld answers to its four names through argv[0], and the package
    # carries one copy of it.
    foreach ($name in "ld.lld", "ld64.lld", "lld-link") {
        if (-not (Test-Path "$home_dir\bin\$name.exe")) {
            Copy-Item "$home_dir\bin\lld.exe" "$home_dir\bin\$name.exe"
        }
    }

    # CMake installs the sysroot. The package holds the script and the
    # pins, and the pinned CMake stands in when the host has none.
    $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if (-not $cmake) {
        $pins = Get-Content "$home_dir\tools\cmake-pin"
        $cmake_version = (Get-Content "$home_dir\tools\cmake-version").Trim()
        $url = (($pins | Where-Object { $_ -match "^$host_name-url=" }) -split "=", 2)[1]
        $url = $url.Replace("@VERSION@", $cmake_version)
        $digest = (($pins | Where-Object { $_ -match "^$host_name-digest=" }) -split "=", 2)[1]
        Say "installing CMake $cmake_version, which the sysroot step needs"
        Invoke-WebRequest $url -OutFile "$work\cmake.zip"
        $got = (Get-FileHash "$work\cmake.zip" -Algorithm SHA256).Hash.ToLower()
        if ($digest -ne $got) { Fail "CMake has SHA-256 $got, expected $digest" }
        Expand-Archive "$work\cmake.zip" -DestinationPath "$home_dir\tools\cmake"
        $cmake = (Get-ChildItem "$home_dir\tools\cmake" -Recurse -Filter cmake.exe |
                  Select-Object -First 1).FullName
    }

    # The script takes the CRT and the SDK of the Build Tools when this
    # machine has them, which downloads nothing and needs no privilege. It
    # stops and asks for the licence when the machine has neither, because
    # xwin then fetches them from Microsoft.
    Say "laying out the sysroot of $host_name"
    $sysroot = @("-DDEST=$home_dir\sysroot", "-DLLVM_BIN=$home_dir\bin",
                 "-DTARGETS=$host_name")
    $script = "$home_dir\tools\get-sysroot.cmake"
    & $cmake $sysroot -P $script
    if ($LASTEXITCODE -ne 0) {
        Say "this machine holds no Microsoft CRT, which a Windows program"
        Say "links against. xwin fetches about 1 GB of it from Microsoft."
        if (Ask MICROSOFT "Let xwin fetch the CRT and the Windows SDK?") {
            # The progress bar of xwin draws only on a terminal of its
            # own, so CMake writes the command and this shell runs it.
            & $cmake $sysroot "-DACCEPT_LICENSE=yes" "-DSPLAT=script" -P $script
            $splat = "$home_dir\sysroot\.download\splat.cmd"
            if ($LASTEXITCODE -eq 0 -and (Test-Path $splat)) {
                & cmd /c $splat
                & $cmake $sysroot "-DACCEPT_LICENSE=yes" "-DSPLAT=done" -P $script
            }
        } else {
            Say "a Developer Command Prompt of Visual Studio needs neither,"
            Say "because lld-link reads the LIB variable it sets."
        }
    }

    # DESIGN: the PATH names one antic, and it is the one this machine runs
    # without emulation. A package of the other processor is therefore
    # called by its path.
    if ($arch -ne $native) {
        Say "run it with: $home_dir\bin\antic.exe hello.anti -o hello.exe"
        return
    }

    # The PATH of the account lives under HKCU and needs no administrator.
    # The installer writes it only with a yes, and the uninstaller removes
    # what it wrote.
    $bin = "$home_dir\bin"
    $path = [Environment]::GetEnvironmentVariable("PATH", "User")
    if ($path -and ($path -split ";" | Where-Object { $_ -eq $bin })) {
        Say "$bin is already in the PATH of your account"
    } elseif (Ask PATH "Add $bin to the PATH of your account?") {
        $joined = if ($path) { "$path;$bin" } else { $bin }
        [Environment]::SetEnvironmentVariable("PATH", $joined, "User")
        $env:PATH = "$bin;$env:PATH"
        Say "added $bin to the PATH of your account and to this shell"
    } else {
        Say "add this to your PATH yourself:"
        Write-Host "    $bin"
    }
    Say "then compile a program with: antic hello.anti -o hello.exe"
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}
