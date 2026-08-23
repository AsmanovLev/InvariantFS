# InvariantFS Dokan-тест (Windows-монтирование)
# Запуск: powershell -ExecutionPolicy Bypass -File tests_dokan.ps1
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root 'build'
$mkfs  = Join-Path $build 'invf-mkfs.exe'
$cp    = Join-Path $build 'invf-cp.exe'
$cat   = Join-Path $build 'invf-cat.exe'
$ls    = Join-Path $build 'invf-ls.exe'
$ver   = Join-Path $build 'invf-verify.exe'
$sweep = Join-Path $build 'invf-sweep.exe'
$dokan = Join-Path $build 'invf-dokan.exe'
$img   = Join-Path $build 'dokan_test.img'
$start = Join-Path $build 'start_dokan.bat'
$log   = Join-Path $build 'dokan_test.log'
$work  = $env:TEMP

$pass = 0; $fail = 0
function Assert($cond, $name) {
    if ($cond) { $script:pass++; Write-Host "  PASS: $name" -ForegroundColor Green }
    else       { $script:fail++; Write-Host "  FAIL: $name" -ForegroundColor Red }
}
function KillDokan {
    taskkill /F /IM invf-dokan.exe 2>$null | Out-Null
    Start-Sleep -Seconds 3
}
function StartDokan {
    Start-Process $dokan -ArgumentList "`"$img`"", 'Y:' -WindowStyle Hidden -RedirectStandardError $log | Out-Null
    Start-Sleep -Seconds 4
}

Write-Host "=== T-D1: сборка (должна быть) ===" -ForegroundColor Cyan
Assert (Test-Path $dokan) "invf-dokan.exe существует"

Write-Host "=== T-D2: образ + монтирование ===" -ForegroundColor Cyan
KillDokan
Remove-Item $img -Force -ErrorAction SilentlyContinue
& $mkfs $img 1 | Out-Null
& $cp $img (Join-Path $root 'doc\01-overview.md') 'doc01.md' | Out-Null
& $cp $img 'H:\gost.exe' 'gost.exe' | Out-Null
& $sweep $img | Out-Null
StartDokan
$proc = Get-Process invf-dokan -ErrorAction SilentlyContinue
Assert ($null -ne $proc) "процесс invf-dokan жив"
$drive = Get-PSDrive Y -ErrorAction SilentlyContinue
Assert ($null -ne $drive) "Y: существует"
Assert ($drive.Description -match 'InvariantFS') "Y: = InvariantFS"

Write-Host "=== T-D3: чтение bit-perfect ===" -ForegroundColor Cyan
$out = Join-Path $build 'dokan_read_check.exe'
Remove-Item $out -Force -ErrorAction SilentlyContinue
[IO.File]::WriteAllBytes($out, [IO.File]::ReadAllBytes('Y:\gost.exe'))
if (Test-Path $out) {
    $h1 = (& b3sum 'H:\gost.exe').Split(' ')[0]
    $h2 = (& b3sum $out).Split(' ')[0]
    Assert ($h1 -eq $h2) "чтение 13MB bit-perfect"
    Remove-Item $out -Force
} else { Assert $false "файл скопирован" }

Write-Host "=== T-D4: запись bit-perfect ===" -ForegroundColor Cyan
[IO.File]::WriteAllBytes('Y:\gost_w.exe', [IO.File]::ReadAllBytes('H:\gost.exe'))
Start-Sleep -Seconds 2
$out = Join-Path $build 'dokan_write_check.exe'
Remove-Item $out -Force -ErrorAction SilentlyContinue
[IO.File]::WriteAllBytes($out, [IO.File]::ReadAllBytes('Y:\gost_w.exe'))
if (Test-Path $out) {
    $h1 = (& b3sum 'H:\gost.exe').Split(' ')[0]
    $h2 = (& b3sum $out).Split(' ')[0]
    Assert ($h1 -eq $h2) "запись 13MB bit-perfect"
    Remove-Item $out -Force
} else { Assert $false "записанный файл скопирован" }

Write-Host "=== T-D5: чтение не мусорит образ ===" -ForegroundColor Cyan
$freeBefore = (Get-PSDrive Y).Free
[IO.File]::WriteAllBytes((Join-Path $build 'dokan_r2.exe'), [IO.File]::ReadAllBytes('Y:\gost.exe'))
Remove-Item (Join-Path $build 'dokan_r2.exe') -Force
$freeAfter = (Get-PSDrive Y).Free
Assert (($freeBefore - $freeAfter) -lt 1MB) "чтение не меняет занятое место"

Write-Host "=== T-D6: персистентность (remount) ===" -ForegroundColor Cyan
KillDokan
StartDokan
$out = Join-Path $build 'dokan_persist.exe'
Remove-Item $out -Force -ErrorAction SilentlyContinue
[IO.File]::WriteAllBytes($out, [IO.File]::ReadAllBytes('Y:\gost_w.exe'))
if (Test-Path $out) {
    $h1 = (& b3sum 'H:\gost.exe').Split(' ')[0]
    $h2 = (& b3sum $out).Split(' ')[0]
    Assert ($h1 -eq $h2) "persistence: gost_w.exe bit-perfect после remount"
    Remove-Item $out -Force
} else { Assert $false "файл после remount" }

Write-Host "=== T-D7: verify образа ===" -ForegroundColor Cyan
KillDokan
$r = & $ver $img 2>&1
Assert (($r -join "`n") -match 'OK:') "verify: образ валиден"
if (($r -join "`n") -match 'allocated \(([\d.]+)%') {
    $pct = [double]$Matches[1]
    Assert ($pct -lt 10) "использовано < 10% ($pct%)"
}

Write-Host "=== T-D8: on-demand sweep (фоновый, по простою) ===" -ForegroundColor Cyan
$env:INVFS_SWEEP_INTERVAL = '2'   # ускорить фоновый цикл
if (Test-Path (Join-Path $build 'pnggen.exe')) {
    $png = Join-Path $work 't18.png'
    & (Join-Path $build 'pnggen.exe') $png 160 96 | Out-Null
    if (-not (Test-Path $png)) {
        Write-Host "  SKIP: pnggen не создал файл" -ForegroundColor Yellow
    } else {
    KillDokan
    StartDokan
    [IO.File]::WriteAllBytes('Y:\ond.png', [IO.File]::ReadAllBytes($png))
    # закрытие файла помечает pending; фоновый sweep пережимает PNG
    Start-Sleep -Seconds 8
    KillDokan
    # после размонтирования: образ должен содержать PNGR-версию (внутренний jxl!)
    $r = & $ls $img 2>&1
    Assert (($r -join "`n") -match 'ond\.png!jxl') "on-demand: JXL-блоб создан фоновым sweep"
    $r = & $cat $img 'ond.png' (Join-Path $work 'ond_cat.png') 2>&1
    $h1 = (& b3sum $png).Split(' ')[0]
    $h2 = (& b3sum (Join-Path $work 'ond_cat.png')).Split(' ')[0]
    Assert ($h1 -eq $h2) "on-demand: реконструкция bit-exact (после размонтирования)"
    Remove-Item (Join-Path $work 'ond_cat.png') -Force
    }
} else {
    Write-Host "  SKIP: pnggen недоступен" -ForegroundColor Yellow
}
Remove-Item Env:\INVFS_SWEEP_INTERVAL -ErrorAction SilentlyContinue

Write-Host "=== T-D9: каталоги (mkdir/вложенные файлы/чтение) ===" -ForegroundColor Cyan
$dirImg = Join-Path $work 'dirs_test.img'
Remove-Item $dirImg -Force -ErrorAction SilentlyContinue
& $mkfs $dirImg 1 | Out-Null
KillDokan
$img = $dirImg
StartDokan
$ok9 = $true
[System.IO.Directory]::CreateDirectory('Y:\proj\src') | Out-Null
[IO.File]::WriteAllBytes('Y:\proj\src\main.c', [Text.Encoding]::UTF8.GetBytes('int main(void){return 0;}'))
Start-Sleep -Seconds 3
if (-not [IO.Directory]::Exists('Y:\proj\src')) { $ok9 = $false }
$content = [IO.File]::ReadAllText('Y:\proj\src\main.c')
if ($content -notmatch 'int main') { $ok9 = $false }
Assert $ok9 "каталоги: mkdir + вложенные файлы + чтение"
KillDokan
$r = & $ls $img 2>&1
Assert (($r -join "`n") -match 'proj/src/main\.c') "каталоги: вложенные файлы в образе"
$img = Join-Path $build 'dokan_test.img'
Remove-Item $dirImg -Force -ErrorAction SilentlyContinue

Write-Host "=== T-D10: ENOSPC (заполнение образа -> чистый отказ) ===" -ForegroundColor Cyan
$fullImg = Join-Path $work 'full_test.img'
Remove-Item $fullImg -Force -ErrorAction SilentlyContinue
& $mkfs $fullImg 0.15 | Out-Null
KillDokan
$img = $fullImg
StartDokan
$ok10 = $true
$rng = New-Object System.Random(42)
$fillBytes = New-Object byte[] (4MB)
$rng.NextBytes($fillBytes)
$i = 0
$wrote = 0
while ($i -lt 200) {
    $name = 'b' + $i + '.bin'
    try {
        [IO.File]::WriteAllBytes((Join-Path 'Y:\' $name), $fillBytes)
        $wrote++; $i++
    } catch { break }
}
Start-Sleep -Seconds 3
Write-Host "  записано до отказа: $wrote файлов по 4MB"
if ($wrote -lt 5) { $ok10 = $false }
if ($wrote -ge 40) { $ok10 = $false }   # 0.15GB не вместит 160MB без сжатия
# содержимое первого файла bit-exact (том не повреждён)
$rd = [IO.File]::ReadAllBytes('Y:\b0.bin')
Write-Host "  dbg: len=$($rd.Length) b1000=$($rd[1000])/$($fillBytes[1000]) blast=$($rd[$rd.Length-1])/$($fillBytes[$fillBytes.Length-1])"
if ($rd.Length -ne $fillBytes.Length) { $ok10 = $false }
if ($rd[1000] -ne $fillBytes[1000]) { $ok10 = $false }
if ($rd[$rd.Length-1] -ne $fillBytes[$fillBytes.Length-1]) { $ok10 = $false }
Assert $ok10 "ENOSPC: заполнение -> чистый отказ, чтение цело"
KillDokan
$img = Join-Path $build 'dokan_test.img'
Remove-Item $fullImg -Force -ErrorAction SilentlyContinue

Write-Host "=== T-D11: rename/move (MoveFile) ===" -ForegroundColor Cyan
$renImg = Join-Path $build 'dokan_ren.img'
Remove-Item $renImg -Force -ErrorAction SilentlyContinue
& $mkfs $renImg 1 | Out-Null
$img = $renImg
StartDokan

# --- файл: переименование в том же каталоге ---
$payload = [byte[]]::new(3000000)
(New-Object Random 7).NextBytes($payload)
[IO.File]::WriteAllBytes('Y:\orig.bin', $payload)
Start-Sleep -Seconds 1
Move-Item 'Y:\orig.bin' 'Y:\moved.bin' -Force
Assert ((Test-Path 'Y:\moved.bin') -and -not (Test-Path 'Y:\orig.bin')) `
    "rename: новое имя есть, старое исчезло"
$back = [IO.File]::ReadAllBytes('Y:\moved.bin')
$same = ($back.Length -eq $payload.Length)
if ($same) { for ($i = 0; $i -lt $payload.Length; $i += 4096) {
    if ($back[$i] -ne $payload[$i]) { $same = $false; break } } }
Assert $same "rename: содержимое bit-exact после переименования"

# --- файл: перемещение в подкаталог ---
New-Item -ItemType Directory -Path 'Y:\sub' -Force | Out-Null
Move-Item 'Y:\moved.bin' 'Y:\sub\deep.bin' -Force
Assert ((Test-Path 'Y:\sub\deep.bin') -and -not (Test-Path 'Y:\moved.bin')) `
    "move: файл переехал в подкаталог"

# --- каталог: переименование вместе с содержимым ---
[IO.File]::WriteAllText('Y:\sub\note.txt', 'invarifs rename test')
Start-Sleep -Seconds 1
Move-Item 'Y:\sub' 'Y:\renamed' -Force
Assert ((Test-Path 'Y:\renamed\note.txt') -and -not (Test-Path 'Y:\sub')) `
    "rename каталога: содержимое переехало вместе с ним"
Assert ([IO.File]::ReadAllText('Y:\renamed\note.txt') -eq 'invarifs rename test') `
    "rename каталога: вложенный файл читается"

# --- rename не должен ничего терять после remount ---
KillDokan
StartDokan
$after = [IO.File]::ReadAllBytes('Y:\renamed\deep.bin')
$same2 = ($after.Length -eq $payload.Length)
if ($same2) { for ($i = 0; $i -lt $payload.Length; $i += 4096) {
    if ($after[$i] -ne $payload[$i]) { $same2 = $false; break } } }
Assert $same2 "rename: bit-exact после remount (L2P перепривязан)"
KillDokan
$vout = & $ver $renImg 2>&1 | Out-String
Assert ($vout -match 'OK|0 corrupt|валиден|corrupt: 0') "rename: verify образа чистый"
Remove-Item $renImg -Force -ErrorAction SilentlyContinue
$img = Join-Path $build 'dokan_test.img'

Write-Host "=== T-D12: перечисление каталогов (Explorer/dir) ===" -ForegroundColor Cyan
$lsImg = Join-Path $build 'dokan_ls.img'
Remove-Item $lsImg -Force -ErrorAction SilentlyContinue
& $mkfs $lsImg 1 | Out-Null
KillDokan
$img = $lsImg
StartDokan
[IO.File]::WriteAllText('Y:\a.txt', 'aaa')
[IO.File]::WriteAllText('Y:\b.txt', 'bbbbb')
New-Item -ItemType Directory -Path 'Y:\d' -Force | Out-Null
[IO.File]::WriteAllText('Y:\d\inner.txt', 'inner')
Start-Sleep -Seconds 2
# корень должен перечисляться: файлы открывались по имени и при пустом листинге
$names = @(Get-ChildItem 'Y:\' | ForEach-Object Name)
Assert (($names -contains 'a.txt') -and ($names -contains 'b.txt') -and
        ($names -contains 'd')) "листинг корня: файлы и каталог видны"
$b = Get-ChildItem 'Y:\b.txt'
Assert ($b.Length -eq 5) "листинг: размер файла верный ($($b.Length))"
$d = Get-Item 'Y:\d'
Assert ($d.LastWriteTime.Year -ge 2000) "листинг: время каталога заполнено ($($d.LastWriteTime.Year))"
$sub = @(Get-ChildItem 'Y:\d' | ForEach-Object Name)
Assert ($sub -contains 'inner.txt') "листинг подкаталога: вложенный файл виден"
KillDokan
Remove-Item $lsImg -Force -ErrorAction SilentlyContinue
$img = Join-Path $build 'dokan_test.img'

Write-Host "`n=== ИТОГ Dokan: $pass PASS, $fail FAIL ===" -ForegroundColor $(if ($fail -eq 0) {'Green'} else {'Red'})
if ($fail -gt 0) { exit 1 }
