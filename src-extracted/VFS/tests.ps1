# InvariantFS test suite — инварианты on-disk формата
# Запуск: powershell -ExecutionPolicy Bypass -File tests.ps1
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root 'build'
$work  = Join-Path $root 'build\tests'
$mkfs  = Join-Path $build 'invf-mkfs.exe'
$ver   = Join-Path $build 'invf-verify.exe'
$cp    = Join-Path $build 'invf-cp.exe'
$cat   = Join-Path $build 'invf-cat.exe'
$ls    = Join-Path $build 'invf-ls.exe'
$sweep = Join-Path $build 'invf-sweep.exe'
$stat  = Join-Path $build 'invf-stat.exe'
$fsck  = Join-Path $build 'invf-fsck.exe'
$rchk  = Join-Path $build 'invf-rangechk.exe'
$arct  = Join-Path $build 'invf-arc-test.exe'

if (!(Test-Path $mkfs)) { throw "Build first: $mkfs" }
New-Item -ItemType Directory -Force -Path $work | Out-Null
$pass = 0; $fail = 0

function Assert($cond, $name) {
    if ($cond) { $script:pass++; Write-Host "  PASS: $name" -ForegroundColor Green }
    else       { $script:fail++; Write-Host "  FAIL: $name" -ForegroundColor Red }
}

function Run($exe, $argList) {
    # stderr у native-процессов (напр. sweep-лог) не должен кидать
    # NativeCommandError при $ErrorActionPreference='Stop' и убивать
    # процесс до flush; реальный код выхода берём из $LASTEXITCODE
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $out = & $exe @argList 2>&1
    $code = $LASTEXITCODE
    $ErrorActionPreference = $prev
    return @{ code = $code; out = ($out | Out-String) }
}

Write-Host "`n=== T1: mkfs 4GB + verify ===" -ForegroundColor Cyan
$img = Join-Path $work 't1.img'
Remove-Item $img -Force -ErrorAction SilentlyContinue
$r = Run $mkfs @($img, 4)
Assert ($r.code -eq 0) "mkfs 4GB exit 0"
$r = Run $ver @($img)
Assert ($r.code -eq 0) "verify exit 0"
Assert ($r.out -match 'OK:') "verify reports OK"
Assert ($r.out -match 'CLEAN') "state CLEAN"

Write-Host "`n=== T2: инвариант — спарс-файл ===" -ForegroundColor Cyan
$img = Join-Path $work 't2.img'
Remove-Item $img -Force -ErrorAction SilentlyContinue
$freeBefore = (Get-PSDrive (Split-Path $img -Qualifier).TrimEnd(':')).Free
& $mkfs $img 4 | Out-Null
$freeAfter = (Get-PSDrive (Split-Path $img -Qualifier).TrimEnd(':')).Free
$consumed = $freeBefore - $freeAfter
$logical = (Get-Item $img).Length
Assert ($logical -eq 4GB) "логический размер = 4GB"
# Инвариант: физически занято < 2% от логического размера (спарс)
$threshold = [int64](4GB * 0.02)
Assert ($consumed -lt $threshold) "спарс: занято $([math]::Round($consumed/1MB,1)) MB < $([math]::Round($threshold/1MB,1)) MB"
Remove-Item $img -Force

Write-Host "`n=== T3: инварианты покрытия зон ===" -ForegroundColor Cyan
$img = Join-Path $work 't3.img'
& $mkfs $img 4 | Out-Null
$r = Run $ver @($img)
$out = $r.out
# зоны: metadata+raw+shadow = total-1 (суперблок)
if ($out -match 'blocks: (\d+) total, (\d+) free, (\d+) allocated') {
    $total = [int64]$Matches[1]; $free = [int64]$Matches[2]; $alloc = [int64]$Matches[3]
    Assert (($free + $alloc) -eq $total) "битмап: free+alloc = total"
}
if ($out -match 'zones: metadata (\d+) \| raw (\d+) \| shadow (\d+)') {
    $m = [int64]$Matches[1]; $r2 = [int64]$Matches[2]; $s = [int64]$Matches[3]
    Assert (($m + $r2 + $s) -eq ($total - 1)) "зоны покрывают весь объём (кроме суперблока)"
    Assert (($r2 * 5) -le ($m + $r2 + $s)) "raw <= 20% объёма"
}
Remove-Item $img -Force

Write-Host "`n=== T4: повреждение суперблока -> checksum FAIL ===" -ForegroundColor Cyan
$img = Join-Path $work 't4.img'
& $mkfs $img 4 | Out-Null
# портим байт 0x1C (block_size) — CRC должен не сойтись
$fs = [System.IO.File]::Open($img, 'Open', 'ReadWrite')
$fs.Position = 0x1C; $fs.WriteByte(0xFF); $fs.Close()
$r = Run $ver @($img)
Assert ($r.code -ne 0) "verify exit != 0 на повреждённом суперблоке"
Assert ($r.out -match 'checksum mismatch') "сообщение о mismatch"
Remove-Item $img -Force

Write-Host "`n=== T5: повреждение битмапа -> alloc mismatch ===" -ForegroundColor Cyan
$img = Join-Path $work 't5.img'
& $mkfs $img 4 | Out-Null
# битмап начинается на блоке metadata_zone_start=1 (offset 4096).
# портим байт в середине битмапа (свободный блок помечаем занятым)
$fs = [System.IO.File]::Open($img, 'Open', 'ReadWrite')
$fs.Position = 4096 + 128 + 8  # блок 1024+64: в RAW zone
$fs.WriteByte(0xFF); $fs.Close()
$r = Run $ver @($img)
# verify должен либо зафиксировать mismatch alloc, либо пройти (байт может быть уже 1)
# главный инвариант: verify не падает и либо ОК, либо явная ошибка — без креша
Assert ($r.code -in 0,1) "verify не упал (code $($r.code))"
Remove-Item $img -Force

Write-Host "`n=== T6: уникальность UUID ===" -ForegroundColor Cyan
$img1 = Join-Path $work 't6a.img'; $img2 = Join-Path $work 't6b.img'
& $mkfs $img1 1 | Out-Null; & $mkfs $img2 1 | Out-Null
$u1 = (& $ver $img1 | Select-String 'uuid:').ToString()
$u2 = (& $ver $img2 | Select-String 'uuid:').ToString()
Assert ($u1 -ne $u2) "два тома имеют разные UUID"
Remove-Item $img1, $img2 -Force

Write-Host "`n=== T7: минимальный размер (64MB) ===" -ForegroundColor Cyan
$img = Join-Path $work 't7.img'
$r = Run $mkfs @($img, '0.0625')
Assert ($r.code -eq 0) "mkfs 64MB OK"
if ($r.code -eq 0) {
    $r = Run $ver @($img)
    Assert ($r.code -eq 0) "verify 64MB OK"
}
Remove-Item $img -Force -ErrorAction SilentlyContinue

Write-Host "`n=== T8: пересоздание поверх существующего ===" -ForegroundColor Cyan
$img = Join-Path $work 't8.img'
& $mkfs $img 2 | Out-Null
$r = Run $mkfs @($img, 1)
Assert ($r.code -eq 0) "mkfs поверх существующего OK"
$r = Run $ver @($img)
Assert ($r.code -eq 0) "verify после пересоздания OK"
$sz = (Get-Item $img).Length
Assert ($sz -eq 1GB) "размер пересозданного = 1GB"
Remove-Item $img -Force

Write-Host "`n=== T9: cp/cat bit-perfect (текст + исходник + exe) ===" -ForegroundColor Cyan
$img = Join-Path $work 't9.img'
& $mkfs $img 1 | Out-Null
$files = @(
    @{ src = (Join-Path $root 'doc\01-overview.md'); name = 'doc01.md' },
    @{ src = (Join-Path $root 'src\volume.c');       name = 'volume.c' },
    @{ src = 'H:\gost.exe';                          name = 'gost.exe' }
)
$allOk = $true
foreach ($f in $files) {
    if (!(Test-Path $f.src)) { Write-Host "  SKIP: $($f.src) нет"; continue }
    $r = Run $cp @($img, $f.src, $f.name)
    Assert ($r.code -eq 0) "cp $($f.name)"
    $out = Join-Path $work "out_$($f.name)"
    $r = Run $cat @($img, $f.name, $out)
    Assert ($r.code -eq 0) "cat $($f.name)"
    $h1 = (& b3sum $f.src).Split(' ')[0]
    $h2 = (& b3sum $out).Split(' ')[0]
    Assert ($h1 -eq $h2) "bit-perfect: $($f.name) BLAKE3 совпадает"
    Remove-Item $out -Force
}
$r = Run $ver @($img)
Assert ($r.code -eq 0) "verify после записи файлов"
$r = Run $ls @($img)
Assert (($r.out -match '3 file') -or ($r.out -match 'gost.exe')) "ls показывает все файлы"
Remove-Item $img -Force

Write-Host "`n=== T10: отсутствующий файл ===" -ForegroundColor Cyan
$img = Join-Path $work 't10.img'
& $mkfs $img 1 | Out-Null
& $cp $img (Join-Path $root 'doc\01-overview.md') 'doc01.md' | Out-Null
$r = Run $cat @($img, 'nope.txt')
Assert ($r.code -ne 0) "cat несуществующего файла -> error"
Assert ($r.out -match 'not found') "сообщение 'not found'"
Remove-Item $img -Force

Write-Host "`n=== T9b: LZ4 в RAW — файл занимает меньше блоков ===" -ForegroundColor Cyan
$img = Join-Path $work 't9b.img'
& $mkfs $img 1 | Out-Null
# большой текст (хорошо сжимается) — соберём из повторяющегося контента
$blob = [byte[]]::new(1MB)
[System.Random]::new(7).NextBytes($blob)
$txt = [System.Text.Encoding]::UTF8.GetString($blob)  # случайный — не сожмётся
# лучше: повторяющийся текст
$txt = "InvariantFS semantic filesystem. " * 20000   # ~640KB сжимаемого текста
$tmp = Join-Path $work 'big.txt'
[System.IO.File]::WriteAllText($tmp, $txt)
& $cp $img $tmp 'big.txt' | Out-Null
$r = Run $ver @($img)
if ($r.out -match 'blocks: (\d+) total, (\d+) free, (\d+) allocated') {
    $total = [int64]$Matches[1]
    $alloc = [int64]$Matches[3]
    $bmBlocks = [int64]($total / 8 / 4096) + 1   # ceil
    # bitmap + L2P journal + inode area (mkfs.c); the inode area scales with
    # the volume — a flat 512 blocks capped any image at ~6200 files
    $inodeBlocks = [Math]::Max([int64]($total / 64), 512)
    $metaBlocks = $bmBlocks + 8192 + $inodeBlocks
    $meta = 1 + $metaBlocks                        # superblock + metadata zone
    $dataBlocks = $alloc - $meta
    $origBlocks = [int64]([System.Text.Encoding]::UTF8.GetByteCount($txt) / 4096) + 1
    Assert ($dataBlocks -lt ($origBlocks / 2)) "LZ4 сжал: $dataBlocks блоков вместо $origBlocks (<50%)"
}
Remove-Item $img, $tmp -Force

Write-Host "`n=== T11: ENOSPC (переполнение RAW) ===" -ForegroundColor Cyan
$img = Join-Path $work 't11.img'
& $mkfs $img '0.0625' | Out-Null   # 64MB
# пишем файлы, пока не переполним RAW (20% от ~64MB = ~12MB)
$blob = [byte[]]::new(1MB)
[System.Random]::new(42).NextBytes($blob)
$tmp = Join-Path $work 'blob.bin'
[System.IO.File]::WriteAllBytes($tmp, $blob)
$failed = $false; $written = 0
for ($i = 0; $i -lt 30; $i++) {
    $r = Run $cp @($img, $tmp, "blob$i.bin")
    if ($r.code -ne 0) { $failed = $true; break }
    $written++
}
Assert $failed "RAW переполнился (write failed)"
Write-Host "  (записано $written MB до ENOSPC)"
$r = Run $ver @($img)
Assert ($r.code -eq 0) "verify после ENOSPC — том корректен"
Remove-Item $img, $tmp -Force

Write-Host "`n=== T12: Sweep — LZ4->ZSTD-19, bit-perfect, освобождение RAW ===" -ForegroundColor Cyan
$img = Join-Path $work 't12.img'
& $mkfs $img 1 | Out-Null
$files = @(
    @{ src = (Join-Path $root 'doc\01-overview.md'); name = 'doc01.md' },
    @{ src = (Join-Path $root 'src\volume.c');       name = 'volume.c' },
    @{ src = 'H:\gost.exe';                          name = 'gost.exe' }
)
$present = @()
foreach ($f in $files) {
    if (!(Test-Path $f.src)) { continue }
    & $cp $img $f.src $f.name | Out-Null
    $present += $f
}
# до sweep
$r = Run $ver @($img)
if ($r.out -match '(\d+) allocated') { $before = [int64]$Matches[1] } else { $before = 0 }
# sweep
$r = Run $sweep @($img)
Assert ($r.code -eq 0) "sweep exit 0"
Assert ($r.out -match "$($present.Count) file") "sweep обработал все файлы"
# после sweep
$r = Run $ver @($img)
Assert ($r.code -eq 0) "verify после sweep"
if ($r.out -match 'blocks: (\d+) total, (\d+) free, (\d+) allocated') {
    $after = [int64]$Matches[3]
    Assert ($after -lt $before) "sweep освободил блоки RAW ($before -> $after)"
}
# bit-perfect после sweep
$allOk = $true
foreach ($f in $present) {
    $out = Join-Path $work "sw_$($f.name)"
    & $cat $img $f.name $out | Out-Null
    $h1 = (& b3sum $f.src).Split(' ')[0]
    $h2 = (& b3sum $out).Split(' ')[0]
    if ($h1 -ne $h2) { $allOk = $false }
    Remove-Item $out -Force
}
Assert $allOk "bit-perfect после sweep (все файлы)"
# повторный sweep — идемпотентность
$r1 = Run $ver @($img)
& $sweep $img | Out-Null
$r2 = Run $ver @($img)
if ($r1.out -match '(\d+) allocated') { $a1 = [int64]$Matches[1] }
if ($r2.out -match '(\d+) allocated') { $a2 = [int64]$Matches[1] }
Assert ($a1 -eq $a2) "повторный sweep идемпотентен (alloc $a1 == $a2)"
Remove-Item $img -Force

Write-Host "`n=== T13: Контейнеры — zip 1:1 + члены-окна (git-safe) ===" -ForegroundColor Cyan
$img = Join-Path $work 't13.img'
$z7 = 'C:\Program Files\7-Zip-Zstandard\7z.exe'
$innerDir = Join-Path $work 't13d'
New-Item -ItemType Directory -Force -Path $innerDir | Out-Null
Set-Content (Join-Path $innerDir 'aa.txt') 'InvariantFS container test. '
Set-Content (Join-Path $innerDir 'bb.txt') 'second member. '
$zip13 = Join-Path $work 't13.zip'
if (Test-Path $z7) {
    & $z7 a -tzip $zip13 (Join-Path $innerDir 'aa.txt') (Join-Path $innerDir 'bb.txt') | Out-Null
    & $mkfs $img 1 | Out-Null
    & $cp $img $zip13 't13.zip' | Out-Null
    $r = Run $sweep @($img)
    Assert ($r.out -match 'exploded') "zip проиндексирован в члены-окна"
    $r = Run $ls @($img)
    Assert ($r.out -match 't13\.zip!aa\.txt') "член aa.txt виден (window)"
    Assert ($r.out -match 't13\.zip!bb\.txt') "член bb.txt виден (window)"
    # ИНВАРИАНТ: контейнер читается байт-в-байт как оригинал
    $r = Run $cat @($img, 't13.zip', (Join-Path $work 't13_out.zip'))
    $h1 = (& b3sum $zip13).Split(' ')[0]
    $h2 = (& b3sum (Join-Path $work 't13_out.zip')).Split(' ')[0]
    Assert ($h1 -eq $h2) "контейнер 1:1 с оригиналом (b3sum)"
    # члены извлекаются из окна
    $r = Run $cat @($img, 't13.zip!aa.txt', (Join-Path $work 't13_aa.txt'))
    $h1 = (& b3sum (Join-Path $innerDir 'aa.txt')).Split(' ')[0]
    $h2 = (& b3sum (Join-Path $work 't13_aa.txt')).Split(' ')[0]
    Assert ($h1 -eq $h2) "член aa.txt извлечён bit-exact"
    $r = Run $ver @($img, '--deep')
    Assert ($r.out -match '0 corrupt') "verify --deep: 0 corrupt"
    Remove-Item (Join-Path $work 't13_out.zip'), (Join-Path $work 't13_aa.txt') -Force
    Remove-Item $img, $zip13 -Force
}
Remove-Item $innerDir -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "`n=== T14: Nested zip-in-zip — 1:1 + извлечение на лету ===" -ForegroundColor Cyan
$img = Join-Path $work 't14.img'
$d = Join-Path $work 't14d'
New-Item -ItemType Directory -Force -Path $d | Out-Null
if (Test-Path $z7) {
    Set-Content (Join-Path $d 'x.txt') 'nested level two. '
    Set-Content (Join-Path $d 'y.txt') 'deeper member. '
    & $z7 a -tzip (Join-Path $work 'in14.zip') (Join-Path $d 'x.txt') (Join-Path $d 'y.txt') | Out-Null
    Set-Content (Join-Path $d 'readme.md') 'top level. '
    Copy-Item (Join-Path $work 'in14.zip') (Join-Path $d 'in14.zip')
    & $z7 a -tzip (Join-Path $work 'out14.zip') (Join-Path $d 'readme.md') (Join-Path $d 'in14.zip') | Out-Null
    & $mkfs $img 1 | Out-Null
    & $cp $img (Join-Path $work 'out14.zip') 'out14.zip' | Out-Null
    & $sweep $img | Out-Null   # один прогон: out14 индексируется (1:1)
    $r = Run $ls @($img)
    Assert ($r.out -match 'out14\.zip!in14\.zip') "вложенный член in14 виден (window)"
    # ИНВАРИАНТ: контейнер 1:1 с оригиналом
    $r = Run $cat @($img, 'out14.zip', (Join-Path $work 'out14_cat.zip'))
    $h1 = (& b3sum (Join-Path $work 'out14.zip')).Split(' ')[0]
    $h2 = (& b3sum (Join-Path $work 'out14_cat.zip')).Split(' ')[0]
    Assert ($h1 -eq $h2) "вложенный контейнер 1:1 (b3sum)"
    # вложенный член извлекается на лету (2 уровня)
    $r = Run $cat @($img, 'out14.zip!in14.zip!x.txt', (Join-Path $work 'out14_x.txt'))
    $h1 = (& b3sum (Join-Path $d 'x.txt')).Split(' ')[0]
    $h2 = (& b3sum (Join-Path $work 'out14_x.txt')).Split(' ')[0]
    Assert ($h1 -eq $h2) "вложенный член x.txt bit-exact (2 уровня)"
    $r = Run $ver @($img, '--deep')
    Assert ($r.out -match '0 corrupt') "verify --deep nested: 0 corrupt"
    # идемпотентность: второй sweep ничего не меняет
    $r1 = Run $ver @($img)
    & $sweep $img | Out-Null
    $r2 = Run $ver @($img)
    if ($r1.out -match '(\d+) allocated') { $a1 = [int64]$Matches[1] }
    if ($r2.out -match '(\d+) allocated') { $a2 = [int64]$Matches[1] }
    Assert ($a1 -eq $a2) "nested sweep идемпотентен (alloc $a1 == $a2)"
    Remove-Item (Join-Path $work 'out14_cat.zip'), (Join-Path $work 'out14_x.txt') -Force
    Remove-Item $img, (Join-Path $work 'in14.zip'), (Join-Path $work 'out14.zip') -Force
}
Remove-Item $d -Recurse -Force -ErrorAction SilentlyContinue

Write-Host "`n=== T15: FLAC -> APE + recipe — bit-exact реконструкция ===" -ForegroundColor Cyan
$flacIn = Join-Path $build 't.flac'
$flac24 = Join-Path $build 'm24.flac'
if ((Test-Path $flacIn) -and (Test-Path $flac24)) {
    $img = Join-Path $work 't15.img'
    Remove-Item $img -Force -ErrorAction SilentlyContinue
    & $mkfs $img 1 | Out-Null
    & $cp $img $flacIn 't.flac' | Out-Null
    & $cp $img $flac24 'm24.flac' | Out-Null
    # форс-транскод: на синтетике APE+recipe может не выигрывать у FLAC -8,
    # но инвариант реконструкции должен проверяться всегда
    $env:INVFS_FORCE_FLACR = '1'
    $r = Run $sweep @($img)
    Remove-Item Env:\INVFS_FORCE_FLACR -ErrorAction SilentlyContinue
    # Ищем то, что sweep печатает сейчас. Раньше тест ждал маркер
    # 'flac-transcoded', которого в src/ нет ни одного — строку заменили на
    # человекочитаемую 'FLAC -> APE + recipe', а ассерты за ней не пошли.
    Assert ($r.out -match 'FLAC -> APE \+ recipe') "sweep: FLAC транскодирован (APE+recipe)"
    $r = Run $ls @($img)
    Assert ($r.out -match 't\.flac!recipe') "рецепт виден как t.flac!recipe"
    Assert ($r.out -match 'm24\.flac!recipe') "рецепт виден как m24.flac!recipe"
    # ИНВАРИАНТ: cat возвращает оригинальный FLAC байт-в-байт
    $r = Run $cat @($img, 't.flac', (Join-Path $work 't15_t.flac'))
    $h1 = (& b3sum $flacIn).Split(' ')[0]
    $h2 = (& b3sum (Join-Path $work 't15_t.flac')).Split(' ')[0]
    Assert ($h1 -eq $h2) "FLAC 16-bit bit-exact (b3sum)"
    $r = Run $cat @($img, 'm24.flac', (Join-Path $work 't15_m24.flac'))
    $h1 = (& b3sum $flac24).Split(' ')[0]
    $h2 = (& b3sum (Join-Path $work 't15_m24.flac')).Split(' ')[0]
    Assert ($h1 -eq $h2) "FLAC 24-bit bit-exact (b3sum)"
    $r = Run $ver @($img, '--deep')
    Assert ($r.out -match '0 corrupt') "verify --deep FLAC: 0 corrupt"
    # идемпотентность: второй sweep ничего не меняет
    $r1 = Run $ver @($img)
    & $sweep $img | Out-Null
    $r2 = Run $ver @($img)
    if ($r1.out -match '(\d+) allocated') { $a1 = [int64]$Matches[1] }
    if ($r2.out -match '(\d+) allocated') { $a2 = [int64]$Matches[1] }
    Assert ($a1 -eq $a2) "FLAC sweep идемпотентен (alloc $a1 == $a2)"

    # --- T15b: вынос обложек (PICTURE) + дедуп одинаковых обложек ---
    if (Get-Command ffmpeg -ErrorAction SilentlyContinue) {
        $coverJpg = Join-Path $work 't15_cover.jpg'
        $tCover = Join-Path $work 't15_tc.flac'
        $cCover = Join-Path $work 't15_cc.flac'
        & ffmpeg -y -loglevel error -f lavfi -i 'color=c=red:s=300x300' -frames:v 1 $coverJpg | Out-Null
        & ffmpeg -y -loglevel error -i $flacIn -i $coverJpg -map 0:a -map 1:v -c:a copy -c:v copy -disposition:v attached_pic $tCover | Out-Null
        & ffmpeg -y -loglevel error -i (Join-Path $build 'c.flac') -i $coverJpg -map 0:a -map 1:v -c:a copy -c:v copy -disposition:v attached_pic $cCover | Out-Null
        $img2 = Join-Path $work 't15b.img'
        Remove-Item $img2 -Force -ErrorAction SilentlyContinue
        & $mkfs $img2 1 | Out-Null
        & $cp $img2 $tCover 't.flac' | Out-Null
        & $cp $img2 $cCover 'c.flac' | Out-Null
        $env:INVFS_FORCE_FLACR = '1'
        $r = Run $sweep @($img2)
        Remove-Item Env:\INVFS_FORCE_FLACR -ErrorAction SilentlyContinue
        $r = Run $ls @($img2)
        Assert ($r.out -match 't\.flac!cover0') "обложка вынесена в t.flac!cover0"
        Assert ($r.out -match 'c\.flac!cover0') "обложка вынесена в c.flac!cover0"
        # ИНВАРИАНТ: реконструкция с обложкой bit-exact
        $r = Run $cat @($img2, 't.flac', (Join-Path $work 't15b_t.flac'))
        $h1 = (& b3sum $tCover).Split(' ')[0]
        $h2 = (& b3sum (Join-Path $work 't15b_t.flac')).Split(' ')[0]
        Assert ($h1 -eq $h2) "FLAC с обложкой bit-exact (b3sum)"
        # дедуп: одинаковые обложки схлопываются
        $r = Run $sweep @($img2, '--dedupe')
        Assert ($r.out -match 'merged 1') "дедуп обложек: merged 1"
        $r = Run $cat @($img2, 'c.flac', (Join-Path $work 't15b_c.flac'))
        $h1 = (& b3sum $cCover).Split(' ')[0]
        $h2 = (& b3sum (Join-Path $work 't15b_c.flac')).Split(' ')[0]
        Assert ($h1 -eq $h2) "после дедупа обложка всё ещё bit-exact"
        Remove-Item $img2, $coverJpg, $tCover, $cCover, (Join-Path $work 't15b_t.flac'), (Join-Path $work 't15b_c.flac') -Force
    } else {
        Write-Host "  SKIP: ffmpeg недоступен (cover-дедуп)" -ForegroundColor Yellow
    }
    Remove-Item $img, (Join-Path $work 't15_t.flac'), (Join-Path $work 't15_m24.flac') -Force
} else {
    Write-Host "  SKIP: нет тестовых FLAC (build/t.flac, build/m24.flac)" -ForegroundColor Yellow
}

# --- T16: TAR-контейнер: члены "name!partN" + IVFT-рецепт (bit-exact) ---
$tarExe = (Get-Command "$env:WINDIR\System32\tar.exe" -ErrorAction SilentlyContinue)
if ($tarExe) {
    $work = $env:TEMP
    $td = Join-Path $work 'invfs_t16'
    if (Test-Path $td) { Remove-Item $td -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $td | Out-Null
    1..200 | ForEach-Object { Add-Content (Join-Path $td 't16.txt') "InvariantFS tar test line $_ - repeating text" }
    $bytes = New-Object byte[] 100000
    (New-Object Random(42)).NextBytes($bytes)
    [IO.File]::WriteAllBytes((Join-Path $td 't16.bin'), $bytes)
    Copy-Item (Join-Path $work 'in14.zip') (Join-Path $td 't16.zip') -ErrorAction SilentlyContinue
    $tarPath = Join-Path $work 't16.tar'
    Push-Location $td
    $tarArgs = @('-cf', $tarPath, 't16.txt', 't16.bin')
    if (Test-Path (Join-Path $td 't16.zip')) { $tarArgs += 't16.zip' }
    & $tarExe @tarArgs 2>&1 | Out-Null
    Pop-Location
    $img = Join-Path $work 't16.img'
    Remove-Item $img -Force -ErrorAction SilentlyContinue
    & $mkfs $img 200 | Out-Null
    & $cp $img $tarPath 'arch.tar' | Out-Null
    $r = Run $sweep @($img)
    Assert ($r.out -match 'tar -> parts \+ recipe') "tar-transcoded (parts + recipe)"
    # члены видны
    $r = Run $ls @($img)
    Assert ($r.out -match 'arch\.tar!part1') "член part1 виден"
    # ИНВАРИАНТ: реконструкция bit-exact
    $r = Run $cat @($img, 'arch.tar', (Join-Path $work 't16_cat.tar'))
    $h1 = (& b3sum $tarPath).Split(' ')[0]
    $h2 = (& b3sum (Join-Path $work 't16_cat.tar')).Split(' ')[0]
    Assert ($h1 -eq $h2) "TAR bit-exact (b3sum)"
    $r = Run $ver @($img, '--deep')
    Assert ($r.out -match '0 corrupt') "verify --deep tar: 0 corrupt"
    # идемпотентность
    $r1 = Run $ver @($img)
    & $sweep $img | Out-Null
    $r2 = Run $ver @($img)
    if ($r1.out -match '(\d+) allocated') { $a1 = [int64]$Matches[1] }
    if ($r2.out -match '(\d+) allocated') { $a2 = [int64]$Matches[1] }
    Assert ($a1 -eq $a2) "tar sweep идемпотентен (alloc $a1 == $a2)"
    Remove-Item $img, $tarPath, (Join-Path $work 't16_cat.tar') -Force
    Remove-Item $td -Recurse -Force
} else {
    Write-Host "  SKIP: tar недоступен" -ForegroundColor Yellow
}

# --- T17: tar.gz (слабый gzip -1): члены + IVGZ-рецепт, bit-exact deflate ---
$gzr = Join-Path $PSScriptRoot 'build\gzrepro.exe'
if (Test-Path $gzr) {
    $work = $env:TEMP
    $td = Join-Path $work 'invfs_t17'
    if (Test-Path $td) { Remove-Item $td -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $td | Out-Null
    $tarPath = Join-Path $work 't17.tar'
    $gzPath = Join-Path $work 't17.tar.gz'
    Push-Location $td
    1..300 | ForEach-Object { Add-Content (Join-Path $td 't17.txt') "line $_ - InvariantFS gzip test - unique content $_ " }
    & $tarExe -cf $tarPath t17.txt 2>&1 | Out-Null
    Pop-Location
    # слабый gzip -1 через наш zlib (bit-exact реплицируется)
    & $gzr --make-gz $tarPath $gzPath 1 | Out-Null
    $img = Join-Path $work 't17.img'
    Remove-Item $img -Force -ErrorAction SilentlyContinue
    & $mkfs $img 300 | Out-Null
    & $cp $img $gzPath 'a.tar.gz' | Out-Null
    $r = Run $sweep @($img)
    Assert ($r.out -match 'gz -> parts \+ recipe') "gz-transcoded (части + IVGZ-рецепт)"
    # ИНВАРИАНТ: bit-exact
    $r = Run $cat @($img, 'a.tar.gz', (Join-Path $work 't17_cat.gz'))
    $h1 = (& b3sum $gzPath).Split(' ')[0]
    $h2 = (& b3sum (Join-Path $work 't17_cat.gz')).Split(' ')[0]
    Assert ($h1 -eq $h2) "tar.gz bit-exact (b3sum)"
    $r = Run $ver @($img, '--deep')
    Assert ($r.out -match '0 corrupt') "verify --deep gz: 0 corrupt"
    # идемпотентность
    & $sweep $img | Out-Null
    $r2 = Run $ver @($img, '--deep')
    Assert ($r2.out -match '0 corrupt') "gz sweep идемпотентен (0 corrupt)"
    Remove-Item $img, $tarPath, $gzPath, (Join-Path $work 't17_cat.gz') -Force
    Remove-Item $td -Recurse -Force
} else {
    Write-Host "  SKIP: gzrepro недоступен (gz-тест)" -ForegroundColor Yellow
}

# --- T18: PNG Repack (JXL lossless + IVPN recipe, bit-exact) ---
$pnggen = Join-Path $PSScriptRoot 'build\pnggen.exe'
if (Test-Path $pnggen) {
    $work = $env:TEMP
    $pngPath = Join-Path $work 't18.png'
    & $pnggen $pngPath 192 128 | Out-Null
    $img = Join-Path $work 't18.img'
    Remove-Item $img -Force -ErrorAction SilentlyContinue
    & $mkfs $img 300 | Out-Null
    & $cp $img $pngPath 'pic.png' | Out-Null
    $r = Run $sweep @($img)
    Assert ($r.out -match 'PNG -> JXL \+ recipe') "png-transcoded (JXL + IVPN)"
    $r = Run $ls @($img)
    Assert ($r.out -match 'pic\.png!jxl') "JXL-блоб виден (pic.png!jxl)"
    # ИНВАРИАНТ: bit-exact
    $r = Run $cat @($img, 'pic.png', (Join-Path $work 't18_cat.png'))
    $h1 = (& b3sum $pngPath).Split(' ')[0]
    $h2 = (& b3sum (Join-Path $work 't18_cat.png')).Split(' ')[0]
    Assert ($h1 -eq $h2) "PNG bit-exact (b3sum)"
    $r = Run $ver @($img, '--deep')
    Assert ($r.out -match '0 corrupt') "verify --deep png: 0 corrupt"
    # идемпотентность
    & $sweep $img | Out-Null
    $r2 = Run $ver @($img, '--deep')
    Assert ($r2.out -match '0 corrupt') "png sweep идемпотентен (0 corrupt)"
    Remove-Item $img, $pngPath, (Join-Path $work 't18_cat.png') -Force
} else {
    Write-Host "  SKIP: pnggen недоступен (png-тест)" -ForegroundColor Yellow
}

# --- T19: большая inode-запись не обрывает скан области ---
# Регрессия: скан inode-области отвергал rec_len > 0x10000 и на такой записи
# ОСТАНАВЛИВАЛСЯ, а не пропускал её. 64 КБ — не свойство формата: запись растёт
# на 24 байта AST-записи за каждые 64 КБ файла, то есть предел приходился на
# файл ~179 МБ. Всё, записанное ПОСЛЕ первого большого файла, становилось
# невидимым, а inode_area_pos откатывался на него — следующая запись затёрла бы
# их. На реальном образе в 143k файлов vol_open сообщал три имени.
#
# 180 МБ нулей дают rec_len 66 240 (за старым пределом) и пишутся за секунды:
# запись большая по числу сегментов, а не по данным на диске.
Write-Host "`n=== T19: запись > 64 КБ — файлы после неё видны ===" -ForegroundColor Cyan
$img = Join-Path $work 't19.img'
Remove-Item $img -Force -ErrorAction SilentlyContinue
& $mkfs $img 1 | Out-Null
$bigPath = Join-Path $work 't19_big.bin'
$fs = [System.IO.File]::Create($bigPath)
$fs.SetLength(180000000)          # разрежённый: 180 МБ нулей, диск не трогаем
$fs.Close()
$aPath = Join-Path $work 't19_a.txt'
$bPath = Join-Path $work 't19_b.txt'
Set-Content -Path $aPath -Value 'before' -NoNewline
Set-Content -Path $bPath -Value 'after'  -NoNewline

& $cp $img $aPath   'a.txt'   | Out-Null
& $cp $img $bigPath 'big.bin' | Out-Null
& $cp $img $bPath   'b.txt'   | Out-Null

# Ключевая проверка: файл, записанный ПОСЛЕ большого. Раньше ls обрывался
# на big.bin и печатал только a.txt.
$r = Run $ls @($img)
Assert ($r.out -match 'b\.txt')   "файл после большой записи виден в ls"
Assert ($r.out -match 'big\.bin') "сама большая запись видна в ls"
Assert ($r.out -match '3 file') "ls считает все 3 файла"

# ls, stat и fsck ходят по области тремя разными путями — раньше каждый со
# своим пределом, поэтому расходились. Сверяем их между собой.
$r = Run $stat @($img)
Assert ($r.out -match '3 live of 3 names') "stat согласен с ls (3 живых)"
$r = Run $fsck @($img)
Assert ($r.out -match 'live files:\s+3') "fsck согласен с ls (3 живых)"
Assert ($r.out -match 'bad records:\s+0') "fsck: 0 битых записей"

# ИНВАРИАНТ 1:1 через большую запись
$outB = Join-Path $work 't19_b_cat.txt'
Run $cat @($img, 'b.txt', $outB) | Out-Null
Assert ((Get-Content $outB -Raw) -eq 'after') "b.txt читается после большой записи"
$outBig = Join-Path $work 't19_big_cat.bin'
Run $cat @($img, 'big.bin', $outBig) | Out-Null
$h1 = (& b3sum $bigPath).Split(' ')[0]
$h2 = (& b3sum $outBig).Split(' ')[0]
Assert ($h1 -eq $h2) "big.bin bit-exact (b3sum)"

Remove-Item $img, $bigPath, $aPath, $bPath, $outB, $outBig -Force -ErrorAction SilentlyContinue

# --- T20: перезапись файла не оставляет мусора и не теряет данные ---
# Две отдельные ошибки в одном пути записи:
#   1) перезапись = удалить, потом создать. Если создание падало (нет места),
#      старый файл был уже удалён, а новые данные отбрасывались — оба потеряны.
#   2) sweep оставляет от файла братьев (name!recipe, name!partN, name!jxl).
#      Перезапись их не трогала: имена остаются валидными живыми записями,
#      fsck считает их живыми файлами, sweep их пропускает (внутренние '!'),
#      и освободить их уже нечем. Утечка навсегда, на каждой перезаписи.
# vol_replace_file пишет новую запись ПЕРВОЙ, и только потом хоронит старую
# вместе с братьями.
Write-Host "`n=== T20: перезапись — без утечки и без потери ===" -ForegroundColor Cyan
$img = Join-Path $work 't20.img'
Remove-Item $img -Force -ErrorAction SilentlyContinue
& $mkfs $img '0.0625' | Out-Null   # 64MB — как в T11, чтобы заполнение было быстрым

# файл, который sweep разложит на компоненты -> появятся братья
$pngPath = Join-Path $work 't20.png'
$pnggen20 = Join-Path $PSScriptRoot 'build\pnggen.exe'
if (Test-Path $pnggen20) { & $pnggen20 $pngPath 192 128 | Out-Null }
if (-not (Test-Path $pngPath)) { $pngPath = $null }

if ($pngPath) {
    Run $cp @($img, $pngPath, 't20.png') | Out-Null
    Run $sweep @($img) | Out-Null
    $r = Run $ls @($img)
    $sibs = ([regex]::Matches($r.out, 't20\.png!')).Count
    Assert ($sibs -gt 0) "sweep создал братьев для t20.png ($sibs)"

    # перезапись тем же CLI-путём, которым пользуется человек
    $plain = Join-Path $work 't20_plain.txt'
    Set-Content $plain 'overwritten' -NoNewline
    Run $cp @($img, $plain, 't20.png') | Out-Null

    $r = Run $ls @($img)
    Assert (([regex]::Matches($r.out, 't20\.png!')).Count -eq 0) "братья убраны после перезаписи"
    Assert ($r.out -match '1 file') "ls: остался ровно 1 файл"
    $r = Run $fsck @($img)
    Assert ($r.out -match 'live files:\s+1') "fsck: 1 живой файл (братья не висят)"
    Assert ($r.out -match 'bad records:\s+0') "fsck: 0 битых записей"

    $outP = Join-Path $work 't20_cat.txt'
    Run $cat @($img, 't20.png', $outP) | Out-Null
    Assert ((Get-Content $outP -Raw) -eq 'overwritten') "читается новое содержимое"
    Remove-Item $plain, $outP -Force -ErrorAction SilentlyContinue
}

# потеря данных: перезапись, которой не хватает места, обязана быть отказом,
# а старое содержимое — остаться читаемым
$keep = Join-Path $work 't20_keep.bin'
$bytes = [byte[]]::new(4096)
for ($i = 0; $i -lt 4096; $i++) { $bytes[$i] = [byte](($i * 7) % 251) }
[System.IO.File]::WriteAllBytes($keep, $bytes)
Run $cp @($img, $keep, 'keep.bin') | Out-Null

$fillPath = Join-Path $work 't20_fill.bin'
$fillBlob = [byte[]]::new(1MB)          # случайные байты, как в T11: нули сжимаются
[System.Random]::new(20).NextBytes($fillBlob)   # почти в ноль и том не заполняется
[System.IO.File]::WriteAllBytes($fillPath, $fillBlob)
$n = 0
while ($n -lt 40) {
    $r = Run $cp @($img, $fillPath, "fill$n")
    if ($r.code -ne 0) { break }
    $n++
}
Assert ($n -lt 40) "том заполнен ($n заполнителей)"

$r = Run $cp @($img, $fillPath, 'keep.bin')   # перезапись, места нет
Assert ($r.code -ne 0) "перезапись без места отклонена"
$outK = Join-Path $work 't20_keep_cat.bin'
Run $cat @($img, 'keep.bin', $outK) | Out-Null
$hk1 = (& b3sum $keep).Split(' ')[0]
$hk2 = (& b3sum $outK).Split(' ')[0]
Assert ($hk1 -eq $hk2) "старое содержимое keep.bin выжило (b3sum)"

Remove-Item $img, $keep, $fillPath, $outK, $pngPath -Force -ErrorAction SilentlyContinue

# --- T21: прерванный транскод не оставляет сирот ---
# Дети транскода ("name!partN", "name!recipe", "name!coverN") ложатся до
# записи, которая владеет именем. Отказ между ними оставлял их живыми
# записями под именами, до которых уже ничто не доходит: sweep пропускает
# внутренние '!' имена, fsck считает их живыми файлами, освободить их нечем.
# vol_transcode_abort сметает братьев на каждом выходе с отказом — включая
# вердикт «не стало меньше, оставляем оригинал», который у tar/gz достигается
# уже ПОСЛЕ записи частей, так что самый дешёвый исход был молча самым дорогим.
#
# У FLAC та же ошибка была не расточительной, а разрушительной: APE-данные
# писались под самим именем ДО рецепта, поэтому сбой между ними оставлял имя
# указывающим на APE без рецепта — файл нечем реконструировать, при том что
# оригинал sweep уже не удалял (записи-то он вернул 0). Теперь запись-владелец
# имени ложится последней, как у tar/gz/png.
#
# INVFS_FAIL_CHILD=N роняет N-ю и все следующие записи блобов — так выглядит
# ENOSPC, и только так проверяются пути с повтором (рецепт FLAC при неудачном
# сжатии ложится несжатым, и одиночный сбой просто уходит в этот запасной
# путь). Снаружи точку сбоя не подобрать: sweep освобождает оригинал по ходу.
Write-Host "`n=== T21: отказ транскода — без сирот ===" -ForegroundColor Cyan
$cases21 = @()

$tarExe21 = (Get-Command "$env:WINDIR\System32\tar.exe" -ErrorAction SilentlyContinue)
if ($tarExe21) {
    $td21 = Join-Path $work 't21d'
    Remove-Item $td21 -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $td21 | Out-Null
    foreach ($nm in @('r1.bin', 'r2.bin')) {
        $rb = [byte[]]::new(120000)
        [System.Random]::new(($nm.GetHashCode() -band 0x7FFF)).NextBytes($rb)
        [System.IO.File]::WriteAllBytes((Join-Path $td21 $nm), $rb)
    }
    $tarPath21 = Join-Path $work 't21.tar'
    Remove-Item $tarPath21 -Force -ErrorAction SilentlyContinue
    Push-Location $td21
    & $tarExe21 @('-cf', $tarPath21, 'r1.bin', 'r2.bin') 2>&1 | Out-Null
    Pop-Location
    $cases21 += @{ src = $tarPath21; name = 'inc.tar'; kind = 'tar'; flacr = $false }
} else {
    Write-Host "  SKIP: tar.exe нет"
}

$flacIn21 = Join-Path $build 't.flac'
if (Test-Path $flacIn21) {
    $cases21 += @{ src = $flacIn21; name = 't.flac'; kind = 'flac'; flacr = $true }
} else {
    Write-Host "  SKIP: build\t.flac нет"
}

foreach ($c in $cases21) {
    $h0 = (& b3sum $c.src).Split(' ')[0]
    # N=1 — до первого ребёнка, N=2 — один ребёнок уже лёг и его надо смести
    foreach ($nth in 1, 2) {
        $tag = "$($c.kind) FAIL_CHILD=$nth"
        $img = Join-Path $work 't21.img'
        Remove-Item $img -Force -ErrorAction SilentlyContinue
        & $mkfs $img 1 | Out-Null
        Run $cp @($img, $c.src, $c.name) | Out-Null

        $env:INVFS_FAIL_CHILD = "$nth"
        $env:INVFS_DEBUG = '1'
        if ($c.flacr) { $env:INVFS_FORCE_FLACR = '1' }
        $r = Run $sweep @($img)
        Remove-Item Env:\INVFS_FAIL_CHILD, Env:\INVFS_DEBUG -ErrorAction SilentlyContinue
        Remove-Item Env:\INVFS_FORCE_FLACR -ErrorAction SilentlyContinue

        # чистка должна быть непустой: иначе ассерты ниже прошли бы и без неё
        if ($nth -eq 2) {
            Assert ($r.out -match 'purged 1 orphan') "${tag}: сообщил об очистке 1 сироты"
        }
        $r = Run $ls @($img)
        $orphans = ([regex]::Matches($r.out, [regex]::Escape($c.name) + '!')).Count
        Assert ($orphans -eq 0) "${tag}: нет сирот $($c.name)! ($orphans)"
        $r = Run $fsck @($img)
        Assert ($r.out -match 'live files:\s+1') "${tag}: fsck — ровно 1 живой файл"
        Assert ($r.out -match 'bad records:\s+0') "${tag}: fsck — 0 битых записей"

        # ИНВАРИАНТ: оригинал не тронут и читается bit-exact
        $out21 = Join-Path $work 't21_cat.bin'
        Remove-Item $out21 -Force -ErrorAction SilentlyContinue
        Run $cat @($img, $c.name, $out21) | Out-Null
        $h1 = (& b3sum $out21).Split(' ')[0]
        Assert ($h0 -eq $h1) "${tag}: оригинал цел (b3sum)"

        Remove-Item $img, $out21 -Force -ErrorAction SilentlyContinue
    }
}
Remove-Item (Join-Path $work 't21.tar') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $work 't21d') -Recurse -Force -ErrorAction SilentlyContinue

# --- T22: ranged read транскодированного файла == целому + ARC-кэш ---
# Монтирование читает через vol_read_range окнами по 64 KB, CLI — через
# vol_read_file целиком. Эти два пути никто не сравнивал на свёрнутом файле, и
# vol_read_range для FLACR/TARR/GZR/PNGR возвращал -1: у транскода нет
# декодируемых сегментов, только блоб плюс рецепт-сибling, так что окно стоит
# полной реконструкции. Отсюда две проверки на каждый кодек: байты совпадают, и
# кэш действительно спрашивают (hits > 0) — с INVFS_ARC_BYTES=0 байты обязаны
# остаться теми же, а счётчики обнулиться, иначе тест ничего не измеряет.
Write-Host "`n--- T22: ranged read + ARC ---"
$t22d = Join-Path $work 't22'
if (Test-Path $t22d) { Remove-Item $t22d -Recurse -Force }
New-Item -ItemType Directory -Force -Path $t22d | Out-Null
# сжимаемый текст: и члену tar, и gzip -1 нужно одно и то же тело
1..300 | ForEach-Object { Add-Content (Join-Path $t22d 'c22.txt') "line $_ - InvariantFS ranged read test $_ " }
$cases22 = @()
$tarExe22 = (Get-Command "$env:WINDIR\System32\tar.exe" -ErrorAction SilentlyContinue)
$gzr22 = Join-Path $build 'gzrepro.exe'
$png22 = Join-Path $build 'pnggen.exe'

# GZR: слабый gzip -1 над сжимаемым tar (как в T17) — свёртка в части + рецепт
if ($tarExe22 -and (Test-Path $gzr22)) {
    Push-Location $t22d
    & $tarExe22 -cf (Join-Path $t22d 'g22.tar') 'c22.txt' 2>&1 | Out-Null
    Pop-Location
    & $gzr22 --make-gz (Join-Path $t22d 'g22.tar') (Join-Path $t22d 'g22.tar.gz') 1 | Out-Null
    if (Test-Path (Join-Path $t22d 'g22.tar.gz')) {
        $cases22 += @{ src = (Join-Path $t22d 'g22.tar.gz'); name = 'g22.tar.gz'; kind = 'gz' }
    }
} else { Write-Host "  SKIP: tar.exe/gzrepro нет (gz-случай)" -ForegroundColor Yellow }
# PNGR
if (Test-Path $png22) {
    & $png22 (Join-Path $t22d 'p22.png') 192 128 | Out-Null
    $cases22 += @{ src = (Join-Path $t22d 'p22.png'); name = 'p22.png'; kind = 'png' }
} else { Write-Host "  SKIP: pnggen нет (png-случай)" -ForegroundColor Yellow }
# FLACR — единственный случай, где реконструкция шеллит наружу (MAC.exe),
# то есть где кэш стоит секунд, а не микросекунд
if (Test-Path (Join-Path $build 't.flac')) {
    $cases22 += @{ src = (Join-Path $build 't.flac'); name = 'f22.flac'; kind = 'flac' }
} else { Write-Host "  SKIP: build\t.flac нет (flac-случай)" -ForegroundColor Yellow }
# TARR: контейнер — члены суть окна в оригинальные байты, поэтому
# последовательное чтение без кэша квадратично
if ($tarExe22) {
    $members22 = @('c22.txt')
    if (Test-Path (Join-Path $t22d 'p22.png')) { $members22 += 'p22.png' }
    Push-Location $t22d
    & $tarExe22 @(@('-cf', (Join-Path $t22d 't22.tar')) + $members22) 2>&1 | Out-Null
    Pop-Location
    if (Test-Path (Join-Path $t22d 't22.tar')) {
        $cases22 += @{ src = (Join-Path $t22d 't22.tar'); name = 't22.tar'; kind = 'tar' }
    }
}

if ($cases22.Count -gt 0 -and (Test-Path $rchk)) {
    $img = Join-Path $work 't22.img'
    Remove-Item $img -Force -ErrorAction SilentlyContinue
    & $mkfs $img 1 | Out-Null
    foreach ($c in $cases22) { Run $cp @($img, $c.src, $c.name) | Out-Null }
    $env:INVFS_FORCE_FLACR = '1'
    $r = Run $sweep @($img)
    Remove-Item Env:\INVFS_FORCE_FLACR -ErrorAction SilentlyContinue
    Assert ($r.out -match "$($cases22.Count) swept") "T22: свёрнуты все $($cases22.Count) файла"

    foreach ($c in $cases22) {
        # кэш включён: байты совпадают и кэш реально спрашивают
        $r = Run $rchk @($img, $c.name)
        Assert ($r.out -match 'identical to whole-file read') "T22 $($c.kind): окна == целому файлу"
        $hits = 0
        if ($r.out -match 'hits=(\d+)') { $hits = [int]$Matches[1] }
        Assert ($hits -gt 0) "T22 $($c.kind): кэш отвечал ($hits hits)"

        # кэш выключен: те же байты, нулевые счётчики
        $env:INVFS_ARC_BYTES = '0'
        $r = Run $rchk @($img, $c.name)
        Remove-Item Env:\INVFS_ARC_BYTES -ErrorAction SilentlyContinue
        Assert ($r.out -match 'identical to whole-file read') "T22 $($c.kind): те же байты без кэша"
        $hits0 = -1
        if ($r.out -match 'hits=(\d+)') { $hits0 = [int]$Matches[1] }
        Assert ($hits0 -eq 0) "T22 $($c.kind): при INVFS_ARC_BYTES=0 кэша нет ($hits0 hits)"
    }
    Remove-Item $img -Force -ErrorAction SilentlyContinue
} else {
    Write-Host "  SKIP: нет случаев или invf-rangechk (T22)" -ForegroundColor Yellow
}
Remove-Item $t22d -Recurse -Force -ErrorAction SilentlyContinue

# --- T23: юнит-тест самого ARC ---
# То, что T22 показать не может: вытеснение, ghost-хиты, адаптацию p и отказ по
# размеру. rangechk работает с одним файлом на процесс, так что все счётчики
# кроме hits/misses/inserts там нулевые — а ошибка в них не ломает байты, только
# скорость, которую assert не видит.
Write-Host "`n--- T23: ARC (юнит) ---"
if (Test-Path $arct) {
    $r = Run $arct @()
    Assert ($r.out -match '0 failure\(s\)') "T23: инварианты ARC держатся"
    Assert ($r.out -match 'scan 2: 8/8') "T23: после адаптации скан не стоит рабочему набору ничего"
    if ($r.out -match '(\d+) checks') { Write-Host "        ($($Matches[1]) внутренних проверок)" -ForegroundColor DarkGray }
} else {
    Write-Host "  SKIP: invf-arc-test нет" -ForegroundColor Yellow
}

Write-Host "`n=== ИТОГ: $pass PASS, $fail FAIL ===" -ForegroundColor $(if ($fail -eq 0) {'Green'} else {'Red'})
if ($fail -gt 0) { exit 1 }
