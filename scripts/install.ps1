# Installs a Slang toolchain on Windows (docs/PLATFORMS.md, "Установка"):
#
#   irm https://raw.githubusercontent.com/dvislobokov/slang/master/scripts/install.ps1 | iex
#   .\install.ps1 [-Version X.Y.Z] [-Dir $env:USERPROFILE\.slang\toolchain] [-From slang-X-windows-amd64.tar.gz]
#
# Downloads slang-<version>-windows-amd64.tar.gz ($env:SLANG_DIST_URL, default
# the GitHub release), checks its .sha256, unpacks it with tar.exe (built into
# Windows 10 1803+) into <Dir>\slang-<version>-windows-amd64 and adds its bin\
# to the user's PATH. Linking needs clang for the MinGW target (e.g. MSYS2
# mingw-w64-x86_64-clang, or llvm-mingw) on PATH.
# Not runnable on the Linux CI that builds the release; tested by hand.
param(
    [string]$Version = $(if ($env:SLANG_VERSION) { $env:SLANG_VERSION } else { "latest" }),
    [string]$Dir = $(if ($env:SLANG_TOOLCHAIN_DIR) { $env:SLANG_TOOLCHAIN_DIR } else { Join-Path $env:USERPROFILE ".slang\toolchain" }),
    [string]$From = ""
)
$ErrorActionPreference = "Stop"
$hostName = "windows-amd64"
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("slang-install-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    if ($From) {
        $archive = $From
    } else {
        $base = if ($env:SLANG_DIST_URL) { $env:SLANG_DIST_URL } else { "https://github.com/dvislobokov/slang/releases" }
        if ($Version -eq "latest") {
            $Version = (Invoke-RestMethod "$base/latest/download/VERSION").Trim()
        }
        $url = if ($env:SLANG_DIST_URL) { $env:SLANG_DIST_URL } else { "https://github.com/dvislobokov/slang/releases/download/v$Version" }
        $file = "slang-$Version-$hostName.tar.gz"
        $archive = Join-Path $tmp $file
        Write-Host "downloading $url/$file"
        Invoke-WebRequest "$url/$file" -OutFile $archive -UseBasicParsing
        try {
            $want = ((Invoke-RestMethod "$url/$file.sha256") -split '\s+')[0].ToLower()
            $got = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLower()
            if ($want -and $want -ne $got) { throw "checksum mismatch ($got, expected $want)" }
        } catch [System.Net.WebException] { }
    }
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    tar -xzf $archive -C $Dir
    if ($LASTEXITCODE -ne 0) { throw "tar failed" }
    $name = (tar -tzf $archive | Select-Object -First 1).Split('/')[0]
    $bin = Join-Path (Join-Path $Dir $name) "bin"
    $path = [Environment]::GetEnvironmentVariable("Path", "User")
    $parts = @($path -split ';' | Where-Object { $_ -and ($_ -notlike "*\.slang\toolchain\slang-*\bin") })
    [Environment]::SetEnvironmentVariable("Path", (@($bin) + $parts) -join ';', "User")
    Write-Host "installed $name in $Dir; $bin added to the user PATH (open a new terminal)"
    if (-not (Get-Command clang -ErrorAction SilentlyContinue)) {
        Write-Host "note: linking needs clang with the MinGW target (MSYS2: pacman -S mingw-w64-x86_64-clang mingw-w64-x86_64-lld)"
    }
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
