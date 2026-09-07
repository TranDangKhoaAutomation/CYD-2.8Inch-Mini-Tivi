param([Parameter(Mandatory = $true)][string]$ServerDir)

$ServerDir = [IO.Path]::GetFullPath($ServerDir)
$PidFile = Join-Path $ServerDir 'server.pid'
$stopped = $false

if (Test-Path -LiteralPath $PidFile) {
    $pidText = Get-Content -LiteralPath $PidFile -Raw
    $proc = Get-CimInstance Win32_Process -Filter "ProcessId = $pidText" -ErrorAction SilentlyContinue
    if ($proc -and $proc.CommandLine -like '*server.py*') {
        Stop-Process -Id $proc.ProcessId -Force
        Write-Host "Da dung Mini TV server nen (PID $($proc.ProcessId))."
        $stopped = $true
    }
    Remove-Item -LiteralPath $PidFile -Force -ErrorAction SilentlyContinue
}

$listeners = Get-NetTCPConnection -LocalPort 8876 -State Listen -ErrorAction SilentlyContinue
foreach ($listener in $listeners) {
    $proc = Get-CimInstance Win32_Process -Filter "ProcessId = $($listener.OwningProcess)"
    if ($proc -and $proc.CommandLine -like '*server.py*') {
        Stop-Process -Id $listener.OwningProcess -Force
        Write-Host "Da dung Mini TV server (PID $($listener.OwningProcess))."
        $stopped = $true
    }
}

if (-not $stopped) { Write-Host 'Mini TV server khong dang chay.' }
