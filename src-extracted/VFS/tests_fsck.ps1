# InvariantFS fsck/repair tests (Windows)
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$build = Join-Path $root 'build'
$mkfs = Join-Path $build 'invf-mkfs.exe'
$cp   = Join-Path $build 'invf-cp.exe'
$cat  = Join-Path $build 'invf-cat.exe'
$fsck = Join-Path $build 'invf-fsck.exe'
$verify = Join-Path $build 'invf-verify.exe'
$pass = 0; $fail = 0
function Assert($cond, $name) {
    if ($cond) { $script:pass++; Write-Host "  PASS: $name" -ForegroundColor Green }
    else       { $script:fail++; Write-Host "  FAIL: $name" -ForegroundColor Red }
}

Write-Host "=== T-F1: fsck на чистом образе ===" -ForegroundColor Cyan
$img = Join-Path $build 'fsck_t1.img'
Remove-Item $img -Force -ErrorAction SilentlyContinue
& $mkfs $img 0.1 | Out-Null
& $cp $img (Join-Path $root 'doc\01-overview.md') 'doc.md' | Out-Null
& $cp $img (Join-Path $build 'invf-cat.exe') 'cat.exe' | Out-Null
$r = & $fsck $img 2>&1
Assert (($r -join "`n") -match 'OK') "fsck: чистый образ -> OK"
Assert (($r -join "`n") -match 'live files:\s+2') "fsck: 2 живых файла"

Write-Host "=== T-F2: осиротевший блок (ручной бит) -> fsck -f освобождает ===" -ForegroundColor Cyan
$img2 = Join-Path $build 'fsck_t2.img'
Remove-Item $img2 -Force -ErrorAction SilentlyContinue
& $mkfs $img2 0.1 | Out-Null
& $cp $img2 (Join-Path $root 'doc\01-overview.md') 'doc.md' | Out-Null
$before = (& $verify $img2 2>&1 | Select-String 'free').ToString().Trim()
wsl bash -c "python3 /mnt/d/VFS/build/bitflip.py /mnt/d/VFS/build/fsck_t2.img 20000 on" | Out-Null
$r = & $fsck $img2 2>&1
Assert (($r -join "`n") -match 'orphans:\s+1') "fsck: найден 1 осиротевший блок"
$r2 = & $fsck $img2 -f 2>&1
Assert (($r2 -join "`n") -match 'freed') "fsck -f: освободил"
$after = (& $verify $img2 2>&1 | Select-String 'free').ToString().Trim()
Assert ($before -eq $after) "verify: free совпал после ремонта"
$rc = & $verify $img2 2>&1
Assert (($rc -join "`n") -match 'valid') "verify: том валиден"

Write-Host "=== T-F3: повреждённая inode-запись -> файлы после неё живы ===" -ForegroundColor Cyan
$img3 = Join-Path $build 'fsck_t3.img'
Remove-Item $img3 -Force -ErrorAction SilentlyContinue
& $mkfs $img3 0.1 | Out-Null
& $cp $img3 (Join-Path $root 'doc\01-overview.md') 'a.md' | Out-Null
& $cp $img3 (Join-Path $root 'doc\02-on-disk-format.md') 'b.md' | Out-Null
wsl bash -c "python3 /mnt/d/VFS/build/corrupt_crc.py /mnt/d/VFS/build/fsck_t3.img" | Out-Null
$r = & $fsck $img3 2>&1
Assert (($r -join "`n") -match 'bad records:\s+1') "fsck: 1 повреждённая запись"
Assert (($r -join "`n") -match 'live files:\s+1') "fsck: файл после битой записи найден"
$out = Join-Path $build 'fsck_b_out.md'
& cmd /c "`"$cat`" `"$img3`" b.md > `"$out`"" 2>$null
$h1 = (& b3sum $out).Split(' ')[0]
$h2 = (& b3sum (Join-Path $root 'doc\02-on-disk-format.md')).Split(' ')[0]
Assert ($h1 -eq $h2) "чтение b.md bit-perfect (несмотря на битую запись a.md)"
& $fsck $img3 -f | Out-Null
$rc = & $verify $img3 2>&1
Assert (($rc -join "`n") -match 'valid') "verify после fsck -f"

Write-Host "=== T-F4: ENOSPC-образ (94% full) -> fsck согласован ===" -ForegroundColor Cyan
$img4 = Join-Path $build 'fsck_t4.img'
Remove-Item $img4 -Force -ErrorAction SilentlyContinue
& $mkfs $img4 0.15 | Out-Null
$ok = $true
for ($i = 1; $i -lt 400; $i++) {
    if (-not (& $cp $img4 (Join-Path $build 'invf-cat.exe') ("f$i.exe") 2>$null)) { break }
}
$r = & $fsck $img4 2>&1
Assert (($r -join "`n") -match 'OK') "fsck: полный образ -> OK (0 фиксов)"
$r2 = & $fsck $img4 -f 2>&1
Assert (($r2 -join "`n") -match 'OK') "fsck -f: полный образ -> OK"
$rc = & $verify $img4 2>&1
Assert (($rc -join "`n") -match 'valid') "verify: полный образ валиден"

Remove-Item (Join-Path $build 'fsck_t*.img') -Force -ErrorAction SilentlyContinue
Remove-Item (Join-Path $build 'fsck_b_out.md') -Force -ErrorAction SilentlyContinue
Write-Host "`n=== ИТОГ fsck: $pass PASS, $fail FAIL ===" -ForegroundColor $(if ($fail -eq 0) {'Green'} else {'Red'})
exit $fail
