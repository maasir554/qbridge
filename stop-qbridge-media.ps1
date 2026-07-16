$pidFile = "E:\QEMU\logs\qbridge-media.pid"

if (Test-Path -LiteralPath $pidFile) {
    $mediaPid = [int](Get-Content -LiteralPath $pidFile)
    Stop-Process -Id $mediaPid -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $pidFile -Force -ErrorAction SilentlyContinue
}
