# Тест WinFsp монтирования InvariantFS
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root 'build'
$mkfs  = Join-Path $build 'invf-mkfs.exe'
$cp    = Join-Path $build 'invf-cp.exe'
$sweep = Join-Path $build 'invf-sweep.exe'
$mount = Join-Path $build 'invf-mount.exe'
$img   = Join-Path $build 'mount_test.img'
$log   = Join-Path $build 'mount_log.txt'

$pass = 0; $fail = 0
function Assert($cond, $name) {
    if ($cond) { $script:pass++; Write-Host "  PASS: $name" -ForegroundColor Green }
    else       { $script:fail++; Write-Host "  FAIL: $name" -ForegroundColor Red }
}

Write-Host "=== T-M1: подготовка образа ===" -ForegroundColor Cyan
Remove-Item $img -Force -ErrorAction SilentlyContinue
& $mkfs $img 1 | Out-Null
& $cp $img (Join-Path $root 'doc\01-overview.md') 'doc01.md' | Out-Null
& $cp $img 'H:\gost.exe' 'gost.exe' | Out-Null
& $sweep $img | Out-Null

Write-Host "=== T-M2: монтирование на X: ===" -ForegroundColor Cyan
Remove-Item $log -Force -ErrorAction SilentlyContinue
$p = Start-Process $mount -ArgumentList "`"$img`"", 'X:' -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru -WindowStyle Hidden
Start-Sleep -Seconds 3
if ($p.HasExited) {
    Write-Host "  mount process exited early, code $($p.ExitCode)"
    Get-Content "$log.err" -ErrorAction SilentlyContinue
    exit 1
}
Write-Host "  mount PID: $($p.Id), stderr:" -ForegroundColor Yellow
Get-Content "$log.err" -ErrorAction SilentlyContinue | ForEach-Object { Write-Host "    $_" }

Write-Host "=== T-M3: dir X:\ ===" -ForegroundColor Cyan
$dir = cmd /c "dir /b X:\" 2>&1
$dir | ForEach-Object { Write-Host "    $_" }
Assert (($dir -join "`n") -match 'doc01.md') "doc01.md виден"
Assert (($dir -join "`n") -match 'gost.exe') "gost.exe виден"

Write-Host "=== T-M4: чтение файла с тома ===" -ForegroundColor Cyan
$out = Join-Path $build 'mount_out.md'
Remove-Item $out -Force -ErrorAction SilentlyContinue
Copy-Item 'X:\doc01.md' $out -ErrorAction SilentlyContinue
if (Test-Path $out) {
    $h1 = (Get-FileHash (Join-Path $root 'doc\01-overview.md') -Algorithm SHA256).Hash
    $h2 = (Get-FileHash $out -Algorithm SHA256).Hash
    Assert ($h1 -eq $h2) "bit-perfect чтение через WinFsp"
    Remove-Item $out -Force
} else {
    Assert $false "файл скопирован с тома"
}

Write-Host "=== T-M5: размер файла ===" -ForegroundColor Cyan
$fi = Get-Item 'X:\gost.exe' -ErrorAction SilentlyContinue
if ($fi) {
    $orig = (Get-Item 'H:\gost.exe').Length
    Assert ($fi.Length -eq $orig) "gost.exe size $($fi.Length) == $orig"
}

Write-Host "=== T-M6: произвольное чтение (чтение середины) ===" -ForegroundColor Cyan
# прочитаем кусок 4KB из середины gost.exe через .NET
try {
    $fs = [System.IO.File]::Open('X:\gost.exe', 'Open', 'Read')
    $mid = [int]($fs.Length / 2)
    $buf1 = New-Object byte[] 4096
    $fs.Position = $mid
    $fs.Read($buf1, 0, 4096) | Out-Null
    $fs.Close()
    $fs2 = [System.IO.File]::Open('H:\gost.exe', 'Open', 'Read')
    $buf2 = New-Object byte[] 4096
    $fs2.Position = $mid
    $fs2.Read($buf2, 0, 4096) | Out-Null
    $fs2.Close()
    $same = $true
    for ($i = 0; $i -lt 4096; $i++) { if ($buf1[$i] -ne $buf2[$i]) { $same = $false; break } }
    Assert $same "range read (середина файла) bit-perfect"
} catch {
    Assert $false "range read исключение: $($_.Exception.Message)"
}

Write-Host "=== T-M7: размонтирование ===" -ForegroundColor Cyan
Stop-Process $p -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
$gone = -not (Test-Path 'X:\')
Assert $gone "X: размонтирован"
Remove-Item $img -Force -ErrorAction SilentlyContinue

Write-Host "`n=== ИТОГ WinFsp: $pass PASS, $fail FAIL ===" -ForegroundColor $(if ($fail -eq 0) {'Green'} else {'Red'})
if ($fail -gt 0) { exit 1 }
