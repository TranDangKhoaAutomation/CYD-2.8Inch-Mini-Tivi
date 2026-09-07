param(
    [Parameter(Mandatory = $true)][string]$Python,
    [Parameter(Mandatory = $true)][string]$ServerDir
)

$ErrorActionPreference = 'Stop'
$ServerDir = [IO.Path]::GetFullPath($ServerDir)
$PidFile = Join-Path $ServerDir 'server.pid'
$OutLog = Join-Path $ServerDir 'server.log'
$ErrLog = Join-Path $ServerDir 'server-error.log'

if (Test-Path -LiteralPath $PidFile) {
    $existingId = (Get-Content -LiteralPath $PidFile -Raw).Trim()
    $existing = Get-CimInstance Win32_Process -Filter "ProcessId = $existingId" -ErrorAction SilentlyContinue
    if ($existing -and $existing.CommandLine -like '*server.py*') {
        Stop-Process -Id $existing.ProcessId -Force
        Write-Host "Da dung Mini TV server cu (PID $existingId) de nap source moi."
        Start-Sleep -Milliseconds 250
    }
    Remove-Item -LiteralPath $PidFile -Force -ErrorAction SilentlyContinue
}

# A stale/missing PID file must not leave an older server.py instance holding the Mini TV port.
$listeners = Get-NetTCPConnection -LocalPort 8876 -State Listen -ErrorAction SilentlyContinue
foreach ($listener in $listeners) {
    $listenerProc = Get-CimInstance Win32_Process -Filter "ProcessId = $($listener.OwningProcess)" -ErrorAction SilentlyContinue
    if ($listenerProc -and $listenerProc.CommandLine -like '*server.py*') {
        Stop-Process -Id $listener.OwningProcess -Force
        Write-Host "Da dung Mini TV server cu tren port 8876 (PID $($listener.OwningProcess))."
        Start-Sleep -Milliseconds 250
    }
}

$proc = Start-Process -FilePath $Python -ArgumentList 'server.py' -WorkingDirectory $ServerDir -WindowStyle Hidden -RedirectStandardOutput $OutLog -RedirectStandardError $ErrLog -PassThru
$proc.Id | Set-Content -LiteralPath $PidFile -NoNewline -Encoding ascii
Start-Sleep -Milliseconds 750
if ($proc.HasExited) {
    Remove-Item -LiteralPath $PidFile -Force -ErrorAction SilentlyContinue
    Write-Error "Mini TV server khong khoi dong. Xem log: $ErrLog"
    exit 1
}

Write-Host "Mini TV server da chay nen (PID $($proc.Id)). Log: $OutLog"
