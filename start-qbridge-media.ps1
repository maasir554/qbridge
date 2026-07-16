$ErrorActionPreference = "Stop"

$root = "E:\QEMU"
$executable = Join-Path $root "qbridge\build-media\qbridge-media.exe"
$log = Join-Path $root "logs\qbridge-media.log"
$snapshot = Join-Path $root "logs\qbridge-media-frame.jpg"
$pidFile = Join-Path $root "logs\qbridge-media.pid"

Remove-Item -LiteralPath $log, $snapshot, $pidFile -Force -ErrorAction SilentlyContinue

$startInfo = [System.Diagnostics.ProcessStartInfo]::new()
$startInfo.FileName = $executable
$startInfo.Arguments = "--camera 0 --width 640 --height 480 --fps 30 --log `"$log`" --snapshot `"$snapshot`""
$startInfo.UseShellExecute = $false
$startInfo.CreateNoWindow = $true
$startInfo.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
$process = [System.Diagnostics.Process]::Start($startInfo)

Start-Sleep -Milliseconds 1800
$process.Refresh()
if ($process.HasExited) {
    if (Test-Path -LiteralPath $log) {
        Get-Content -LiteralPath $log
    }
    throw "QBridge Media Foundation capture exited before producing frames."
}

Set-Content -LiteralPath $pidFile -Value $process.Id -Encoding ascii
