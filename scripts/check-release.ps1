param(
    [string]$Executable = "$PSScriptRoot\..\dist\LShift.exe",
    [string]$Report = "$PSScriptRoot\..\.cache\release-check.json",
    [switch]$ObserveOnly,
    [switch]$ResourcesOnly
)
$ErrorActionPreference = 'Stop'
$releaseRoot = [IO.Path]::GetFullPath("$PSScriptRoot\..")
$releaseExe = [IO.Path]::GetFullPath($Executable)
$releaseReport = [IO.Path]::GetFullPath($Report)
$releaseTemp = Join-Path $releaseRoot '.cache\temp'
[IO.Directory]::CreateDirectory($releaseTemp) | Out-Null
$env:TEMP = $releaseTemp
$env:TMP = $releaseTemp
$releaseSamples = @()
$releaseRunCount = if ($ResourcesOnly) { 0 } else { 3 }
for ($releaseRun = 0; $releaseRun -lt $releaseRunCount; $releaseRun++) {
    $releaseSmoke = Join-Path $releaseRoot ".cache\release-smoke-$releaseRun.json"
    $releaseChild = Start-Process -FilePath $releaseExe -ArgumentList @('--smoke-test', $releaseSmoke) -WindowStyle Hidden -PassThru
    try {
        Start-Sleep -Milliseconds 650
        $releaseChild.Refresh()
        if ($releaseChild.HasExited) { throw "LShift exited before measurement: $($releaseChild.ExitCode)" }
        $releaseSample = [ordered]@{
            working_set = $releaseChild.WorkingSet64
            private_bytes = $releaseChild.PrivateMemorySize64
            threads = $releaseChild.Threads.Count
            handles = $releaseChild.HandleCount
            cpu_ms = $releaseChild.TotalProcessorTime.TotalMilliseconds
        }
        if (!$releaseChild.WaitForExit(5000)) { throw 'LShift smoke did not finish' }
        if ($releaseChild.ExitCode -ne 0) { throw "LShift smoke failed: $($releaseChild.ExitCode)" }
        $releaseSmokeResult = Get-Content -LiteralPath $releaseSmoke -Raw | ConvertFrom-Json
        if (!$releaseSmokeResult.raw_input_registered -or !$releaseSmokeResult.tray_registered -or
            !$releaseSmokeResult.worker_initialized -or !$releaseSmokeResult.gesture_actions_disabled) {
            throw 'LShift smoke initialization failed'
        }
        $releaseSample.smoke = $releaseSmokeResult
        $releaseSamples += $releaseSample
    } finally {
        if (!$releaseChild.HasExited) { $releaseChild.Kill(); $releaseChild.WaitForExit() }
        $releaseChild.Dispose()
    }
}
$releaseResult = [ordered]@{
    executable = $releaseExe
    bytes = (Get-Item -LiteralPath $releaseExe).Length
    version = (Get-Item -LiteralPath $releaseExe).VersionInfo.FileVersion
    description = (Get-Item -LiteralPath $releaseExe).VersionInfo.FileDescription
    samples = $releaseSamples
}
if (!$ObserveOnly) {
    # Проверяем реальные PE-ресурсы и декодирование значков средствами Windows.
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class LShiftIconCheck {
    [DllImport("kernel32", CharSet=CharSet.Unicode)] public static extern IntPtr LoadLibraryEx(string p, IntPtr f, uint flags);
    [DllImport("kernel32", CharSet=CharSet.Unicode)] public static extern IntPtr FindResource(IntPtr m, IntPtr n, IntPtr t);
    [DllImport("kernel32")] public static extern IntPtr LoadResource(IntPtr m, IntPtr r);
    [DllImport("kernel32")] public static extern IntPtr LockResource(IntPtr r);
    [DllImport("kernel32")] public static extern uint SizeofResource(IntPtr m, IntPtr r);
    [DllImport("kernel32")] public static extern bool FreeLibrary(IntPtr m);
    [DllImport("user32", CharSet=CharSet.Unicode)] public static extern IntPtr LoadImage(IntPtr m, IntPtr n, uint type, int x, int y, uint flags);
    [DllImport("user32")] public static extern bool DestroyIcon(IntPtr i);
}
'@
    $releaseModule = [LShiftIconCheck]::LoadLibraryEx($releaseExe, [IntPtr]::Zero, 0x22)
    if ($releaseModule -eq [IntPtr]::Zero) { throw 'Cannot open PE icon resources' }
    try {
        foreach ($releaseNotice in @(@{id=201;name='LICENSE'}, @{id=202;name='NOTICE'})) {
            $releaseResource = [LShiftIconCheck]::FindResource($releaseModule, [IntPtr]$releaseNotice.id, [IntPtr]10)
            if ($releaseResource -eq [IntPtr]::Zero) { throw "Embedded $($releaseNotice.name) is missing" }
            $releaseBytes = [byte[]]::new([LShiftIconCheck]::SizeofResource($releaseModule, $releaseResource))
            $releaseData = [LShiftIconCheck]::LockResource([LShiftIconCheck]::LoadResource($releaseModule, $releaseResource))
            [Runtime.InteropServices.Marshal]::Copy($releaseData, $releaseBytes, 0, $releaseBytes.Length)
            $releaseSha = [Security.Cryptography.SHA256]::Create()
            try { $releaseHash = [BitConverter]::ToString($releaseSha.ComputeHash($releaseBytes)).Replace('-', '') }
            finally { $releaseSha.Dispose() }
            if ($releaseHash -ne (Get-FileHash -LiteralPath (Join-Path $releaseRoot $releaseNotice.name) -Algorithm SHA256).Hash) {
                throw "Embedded $($releaseNotice.name) does not match the distribution terms"
            }
        }
        $releaseResult.embedded_notices = @('LICENSE', 'NOTICE')
        $releaseGroup = [LShiftIconCheck]::FindResource($releaseModule, [IntPtr]101, [IntPtr]14)
        if ($releaseGroup -eq [IntPtr]::Zero) { throw 'PE icon group is missing' }
        $releasePointer = [LShiftIconCheck]::LockResource([LShiftIconCheck]::LoadResource($releaseModule, $releaseGroup))
        $releaseIconCount = [Runtime.InteropServices.Marshal]::ReadInt16($releasePointer, 4)
        $releaseSizes = @()
        for ($releaseIconIndex = 0; $releaseIconIndex -lt $releaseIconCount; ++$releaseIconIndex) {
            $releaseSize = [Runtime.InteropServices.Marshal]::ReadByte($releasePointer, 6 + 14 * $releaseIconIndex)
            if ($releaseSize -eq 0) { $releaseSize = 256 }
            $releaseIcon = [LShiftIconCheck]::LoadImage($releaseModule, [IntPtr]101, 1, $releaseSize, $releaseSize, 0)
            if ($releaseIcon -eq [IntPtr]::Zero) { throw "Cannot decode $releaseSize pixel icon" }
            [LShiftIconCheck]::DestroyIcon($releaseIcon) | Out-Null
            $releaseSizes += $releaseSize
        }
        if (($releaseSizes -join ',') -ne '16,20,24,32,40,48,64,128,256') { throw 'Incomplete PE icon sizes' }
        $releaseResult.icon_sizes = $releaseSizes
    } finally { [LShiftIconCheck]::FreeLibrary($releaseModule) | Out-Null }
}
$releaseResult | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $releaseReport -Encoding utf8
if (!$ObserveOnly) {
    if ($releaseResult.description -ne 'LShift') { throw 'File description must be LShift' }
    foreach ($releaseSample in $releaseSamples) {
        if ($releaseSample.smoke.com_initialized -ne $false) { throw 'COM must remain deferred before a text command' }
        if ($releaseSample.smoke.diagnostics_enabled -ne $false) { throw 'Release logging must be compiled out' }
    }
}
$releaseResult | ConvertTo-Json -Depth 6
