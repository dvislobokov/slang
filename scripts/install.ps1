# Installs a Zell toolchain on Windows (docs/PLATFORMS.md, "Установка"):
#
#   irm https://raw.githubusercontent.com/zell-lang/zell/master/scripts/install.ps1 | iex
#   .\install.ps1 [-Version X.Y.Z] [-Dir $env:USERPROFILE\.zell\toolchain] [-From zell-X-windows-amd64.tar.gz]
#
# Downloads zell-<version>-windows-amd64.tar.gz ($env:ZELL_DIST_URL, default
# the GitHub release), checks its .sha256, unpacks it with tar.exe (built into
# Windows 10 1803+) into <Dir>\zell-<version>-windows-amd64 and adds its bin\
# to the user's PATH. Linking needs clang for the MinGW target (e.g. MSYS2
# mingw-w64-x86_64-clang, or llvm-mingw) on PATH.
# Not runnable on the Linux CI that builds the release; tested by hand.
param(
    [string]$Version = $(if ($env:ZELL_VERSION) { $env:ZELL_VERSION } else { "latest" }),
    [string]$Dir = $(if ($env:ZELL_TOOLCHAIN_DIR) { $env:ZELL_TOOLCHAIN_DIR } else { Join-Path $env:USERPROFILE ".zell\toolchain" }),
    [string]$From = ""
)
$ErrorActionPreference = "Stop"
$hostName = "windows-amd64"
$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("zell-install-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
    if ($From) {
        $archive = $From
    } else {
        $base = if ($env:ZELL_DIST_URL) { $env:ZELL_DIST_URL } else { "https://github.com/zell-lang/zell/releases" }
        if ($Version -eq "latest") {
            $Version = (Invoke-RestMethod "$base/latest/download/VERSION").Trim()
        }
        $url = if ($env:ZELL_DIST_URL) { $env:ZELL_DIST_URL } else { "https://github.com/zell-lang/zell/releases/download/v$Version" }
        $file = "zell-$Version-$hostName.tar.gz"
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
    $parts = @($path -split ';' | Where-Object { $_ -and ($_ -notlike "*\.zell\toolchain\zell-*\bin") })
    [Environment]::SetEnvironmentVariable("Path", (@($bin) + $parts) -join ';', "User")
    Write-Host "installed $name in $Dir; $bin added to the user PATH (open a new terminal)"
    if (-not (Get-Command clang -ErrorAction SilentlyContinue)) {
        Write-Host "note: linking needs clang with the MinGW target (MSYS2: pacman -S mingw-w64-x86_64-clang mingw-w64-x86_64-lld)"
    }
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
