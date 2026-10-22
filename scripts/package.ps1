$ErrorActionPreference = 'Stop'
$packageRoot = [IO.Path]::GetFullPath("$PSScriptRoot\..")
$packageTemp = Join-Path $packageRoot '.cache\temp'
[IO.Directory]::CreateDirectory($packageTemp) | Out-Null
$env:TEMP = $packageTemp
$env:TMP = $packageTemp
$packageCMake = Get-Content -LiteralPath (Join-Path $packageRoot 'CMakeLists.txt') -Raw
if ($packageCMake -notmatch 'project\(LShift VERSION (\d+\.\d+\.\d+)') { throw 'Project version is missing' }
$packageVersion = $Matches[1]
$packageDist = Join-Path $packageRoot 'dist'
$packageExe = Join-Path $packageDist 'LShift.exe'
if ((Get-Item -LiteralPath $packageExe).VersionInfo.FileVersion -ne $packageVersion) { throw 'EXE version does not match CMake' }
$packageReadme = Join-Path $packageRoot 'resources\README.txt'
Copy-Item -LiteralPath $packageReadme -Destination (Join-Path $packageDist 'README.txt') -Force
$packageFiles = @($packageExe, (Join-Path $packageDist 'README.txt'))
foreach ($packageNotice in @('LICENSE', 'NOTICE')) {
    $packageNoticePath = Join-Path $packageRoot $packageNotice
    if (!(Test-Path -LiteralPath $packageNoticePath)) { throw "Required distribution notice is missing: $packageNotice" }
    $packageFiles += $packageNoticePath
}
$packageArchive = Join-Path $packageDist "LShift-$packageVersion-win-x64.zip"
Compress-Archive -LiteralPath $packageFiles -DestinationPath $packageArchive -Force
Add-Type -AssemblyName System.IO.Compression.FileSystem
$packageZip = [IO.Compression.ZipFile]::OpenRead($packageArchive)
try {
    foreach ($packageFile in $packageFiles) {
        $packageEntry = $packageZip.GetEntry([IO.Path]::GetFileName($packageFile))
        if (!$packageEntry -or $packageEntry.Length -ne (Get-Item -LiteralPath $packageFile).Length) { throw 'ZIP entry is missing or truncated' }
        $packageStream = $packageEntry.Open()
        $packageSha = [Security.Cryptography.SHA256]::Create()
        try { $packageHash = [BitConverter]::ToString($packageSha.ComputeHash($packageStream)).Replace('-', '') }
        finally { $packageStream.Dispose(); $packageSha.Dispose() }
        if ($packageHash -ne (Get-FileHash -LiteralPath $packageFile -Algorithm SHA256).Hash) { throw 'ZIP entry hash mismatch' }
    }
} finally { $packageZip.Dispose() }
$packageResult = [ordered]@{archive=$packageArchive;version=$packageVersion;
    exe_sha256=(Get-FileHash -LiteralPath $packageExe -Algorithm SHA256).Hash;
    zip_sha256=(Get-FileHash -LiteralPath $packageArchive -Algorithm SHA256).Hash}
$packageResult | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $packageRoot '.cache\package.json') -Encoding utf8
$packageResult | ConvertTo-Json
