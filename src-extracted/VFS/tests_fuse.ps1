# InvariantFS FUSE-тест (WSL2 + Void Linux)
# Проверяет: монтирование, чтение, bit-perfect, скорость
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root 'build'

Write-Host "=== T-F1: сборка в WSL ===" -ForegroundColor Cyan
$r = wsl -d void -- bash -c "cd /mnt/d/VFS && gcc -O2 -o build_linux/invf-fuse src/fuse_fs.c src/volume.c src/vol_cpack.c src/vol_png.c src/vol_seal.c src/vol_repair.c src/vol_rollback.c src/vol_resize.c src/vol_fsck.c src/vol_crash.c src/vol_exer.c src/vol_dedupe.c src/vol_textzone.c src/vol_heat.c src/vol_sweep.c src/vol_read.c src/vol_write.c src/vol_records.c src/vol_ast.c src/vol_dirs.c src/arc.c src/crc32c.c src/lz4.c src/flacx.c src/tarx.c src/pngx.c src/miniz.c -lfuse3 -Wl,-l:libzstd.so.1 -lz -Isrc -pthread -DINVFS_EMBED_FLACX -DMINIZ_NO_ZLIB_APIS 2>&1 && echo BUILD-OK"
if ($r -match 'BUILD-OK') { Write-Host "  PASS: сборка" -ForegroundColor Green } else { Write-Host "  FAIL: сборка $r" -ForegroundColor Red; exit 1 }

Write-Host "=== T-F2: образ + монтирование ===" -ForegroundColor Cyan
$img = Join-Path $build 'fuse_test.img'
Remove-Item $img -Force -ErrorAction SilentlyContinue
& (Join-Path $build 'invf-mkfs.exe') $img 1 | Out-Null
& (Join-Path $build 'invf-cp.exe') $img (Join-Path $root 'doc\01-overview.md') 'doc01.md' | Out-Null
& (Join-Path $build 'invf-cp.exe') $img 'H:\gost.exe' 'gost.exe' | Out-Null
& (Join-Path $build 'invf-sweep.exe') $img | Out-Null

$m = wsl -d void -- bash -c "sudo mkdir -p /mnt/invarifs 2>/dev/null; fusermount3 -u /mnt/invarifs 2>/dev/null; sleep 1; setsid /mnt/d/VFS/build_linux/invf-fuse /mnt/d/VFS/build/fuse_test.img /mnt/invarifs >/tmp/fuse.log 2>&1 < /dev/null & sleep 2; ls /mnt/invarifs/ 2>&1"
if ($m -match 'doc01.md' -and $m -match 'gost.exe') {
    Write-Host "  PASS: смонтирован, файлы видны" -ForegroundColor Green
} else {
    Write-Host "  FAIL: mount $m" -ForegroundColor Red
}

Write-Host "=== T-F3: bit-perfect через FUSE ===" -ForegroundColor Cyan
$h = wsl -d void -- bash -c "sha256sum /mnt/invarifs/gost.exe /mnt/h/gost.exe /mnt/invarifs/doc01.md /mnt/d/VFS/doc/01-overview.md 2>&1"
$lines = $h -split "`n" | Where-Object { $_.Trim() -ne '' }
$ok = $true
foreach ($pair in @(@('doc01.md', 'doc01.md'), @('gost.exe', 'gost.exe'))) {
    # простая проверка: первые два хэша в каждой паре
}
if ($lines.Count -eq 4) {
    $h1 = ($lines[0] -split ' ')[0]; $h2 = ($lines[1] -split ' ')[0]
    $h3 = ($lines[2] -split ' ')[0]; $h4 = ($lines[3] -split ' ')[0]
    if ($h1 -eq $h2 -and $h3 -eq $h4) {
        Write-Host "  PASS: bit-perfect (gost.exe + doc01.md)" -ForegroundColor Green
    } else { Write-Host "  FAIL: hash mismatch" -ForegroundColor Red; $ok = $false }
} else { Write-Host "  FAIL: неполный вывод: $($lines.Count) строк" -ForegroundColor Red; $ok = $false }

Write-Host "=== T-F4: скорость чтения ===" -ForegroundColor Cyan
$spd = wsl -d void -- bash -c "dd if=/mnt/invarifs/gost.exe of=/dev/null bs=1M 2>&1 | tail -1"
Write-Host "  $spd"
if ($spd -match '([0-9.]+) MB/s') {
    $mb = [double]$Matches[1]
    if ($mb -gt 50) { Write-Host "  PASS: скорость > 50 MB/s" -ForegroundColor Green }
    else { Write-Host "  FAIL: медленно ($mb MB/s)" -ForegroundColor Red }
}

Write-Host "=== T-F5: размонтирование ===" -ForegroundColor Cyan
wsl -d void -- bash -c "fusermount3 -u /mnt/invarifs 2>&1" | Out-Null
$check = wsl -d void -- bash -c "mountpoint -q /mnt/invarifs 2>&1 && echo STILL-MOUNTED || echo UNMOUNTED"
if ($check -match 'UNMOUNTED') { Write-Host "  PASS: размонтирован" -ForegroundColor Green }
else { Write-Host "  FAIL: $check" -ForegroundColor Red }

Write-Host "=== T-F6: write-путь (create/append/overwrite/unlink) ===" -ForegroundColor Cyan
$img = Join-Path $build 'fuse_test.img'
$r = wsl -d void -- bash -c "
setsid /mnt/d/VFS/build_linux/invf-fuse /mnt/d/VFS/build/fuse_test.img /mnt/invarifs >/tmp/fuse_f6.log 2>&1 </dev/null &
sleep 2
echo 'base' > /mnt/invarifs/w.txt; sleep 1
echo 'append1' >> /mnt/invarifs/w.txt; sleep 1
echo 'append2' >> /mnt/invarifs/w.txt; sync; sleep 2
cat /mnt/invarifs/w.txt
echo '---'
rm /mnt/invarifs/w.txt
ls /mnt/invarifs/ | grep -c w.txt
echo '---'
fusermount3 -u /mnt/invarifs
echo F6-DONE"
$rjoined = ($r -join "`n")
$parts = $rjoined -split '---'
$ok6 = $false
if ($parts.Count -ge 3) {
    $content = $parts[0]
    $countAfter = $parts[1].Trim()
    if ($content -match 'base' -and $content -match 'append1' -and $content -match 'append2' -and $countAfter -eq '0') {
        $ok6 = $true
    }
}
if ($ok6) { Write-Host "  PASS: create+append+unlink" -ForegroundColor Green }
else { Write-Host "  FAIL: write-путь: $r" -ForegroundColor Red }

Write-Host "=== T-F7: write 13MB bit-perfect + remount persistence ===" -ForegroundColor Cyan
$r = wsl -d void -- bash -c "setsid /mnt/d/VFS/build_linux/invf-fuse /mnt/d/VFS/build/fuse_test.img /mnt/invarifs >/tmp/fuse_f7.log 2>&1 </dev/null & sleep 2; cp /mnt/h/gost.exe /mnt/invarifs/g2.exe; fusermount3 -u /mnt/invarifs; sleep 1; setsid /mnt/d/VFS/build_linux/invf-fuse /mnt/d/VFS/build/fuse_test.img /mnt/invarifs >/tmp/fuse_f7b.log 2>&1 </dev/null & sleep 2; sha256sum /mnt/invarifs/g2.exe /mnt/h/gost.exe; fusermount3 -u /mnt/invarifs; echo F7-DONE"
$hashes = [regex]::Matches($r, '[0-9a-f]{64}')
if ($hashes.Count -ge 2 -and $hashes[0].Value -eq $hashes[1].Value) {
    Write-Host "  PASS: write 13MB + persistence bit-perfect" -ForegroundColor Green
} else { Write-Host "  FAIL: $r" -ForegroundColor Red }

Write-Host "`n=== FUSE ИТОГ ===" -ForegroundColor Cyan
