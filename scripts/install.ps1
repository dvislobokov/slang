# Installs a Zell toolchain on Windows (docs/PLATFORMS.md, "Установка"):
#
#   irm https://raw.githubusercontent.com/zell-lang/zell/master/scripts/install.ps1 | iex
#   .\install.ps1 [-Version X.Y.Z] [-Dir $env:USERPROFILE\.zell\toolchain] [-From zell-X-windows-amd64.tar.gz]
#
# Downloads zell-<version>-windows-amd64.tar.xz ($env:ZELL_DIST_URL, default
# the GitHub release; the .tar.gz when this tar.exe cannot read xz), checks its
# .sha256, unpacks it with tar.exe (built into Windows 10 1803+) into
# <Dir>\zell-<version>-windows-amd64 and adds its bin\ to the user's PATH. The
# toolchain is self-contained (bin\lld.exe, the MSVC runtime DLLs and the link
# inputs of every target): nothing else needs to be installed.
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
            $vurl = if ($env:ZELL_DIST_URL) { "$base/VERSION" } else { "$base/latest/download/VERSION" }
            Invoke-WebRequest $vurl -OutFile (Join-Path $tmp "VERSION") -UseBasicParsing
            $Version = (Get-Content (Join-Path $tmp "VERSION") -Raw).Trim()
        }
        $url = if ($env:ZELL_DIST_URL) { $env:ZELL_DIST_URL } else { "https://github.com/zell-lang/zell/releases/download/v$Version" }
        $file = "zell-$Version-$hostName.tar.gz"
        $archive = Join-Path $tmp $file
        Write-Host "downloading $url/$file"
        Invoke-WebRequest "$url/$file" -OutFile $archive -UseBasicParsing
        try {
            Invoke-WebRequest "$url/$file.sha256" -OutFile "$archive.sha256" -UseBasicParsing
            $want = ((Get-Content "$archive.sha256" -Raw) -split '\s+')[0].ToLower()
            $got = (Get-FileHash $archive -Algorithm SHA256).Hash.ToLower()
            if ($want -and $want -ne $got) { throw "checksum mismatch ($got, expected $want)" }
        } catch {
            if ($_.Exception.Message -like "checksum mismatch*") { throw }
            Write-Host "note: no checksum file next to the archive; not verified"
        }
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
    & (Join-Path $bin "zell.exe") doctor *> $null
    if ($LASTEXITCODE -ne 0) { Write-Host "note: ``zell doctor`` reports a problem with the toolchain" }
} finally {
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}
