#!/usr/bin/env bash
# plugin-daemon-smoke.sh — drive the real worker pool daemon end to end:
# invf-plugin-host (shm + eventfd + dlmopen) <- vol_plugin_client <- a
# containerpack .so, over a full enumerate/extract/strip/rebuild cycle, and
# check the rebuilt image against the original bit for bit.
#
# This is the leg tools/test-ivpacks.sh cannot cover: it needs a daemon, and a
# daemon needs 64 MiB of /dev/shm per worker. It is a developer tool, not part
# of `make test`:
#
#   bash tools/plugin-daemon-smoke.sh            # rawdisk GPT fixture, 2 workers
#   PACK=qcow2 bash tools/plugin-daemon-smoke.sh # qcow2 (needs qemu-img)
#   WORKERS=4 SHM_SIZE=1G bash tools/plugin-daemon-smoke.sh
#
# If /dev/shm is too small it says so and exits 0 (SKIP), because a container
# with the default 64 MiB tmpfs simply cannot host the pool.
set -euo pipefail

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$REPO"
PACK="${PACK:-rawdisk}"
WORKERS="${WORKERS:-2}"
SHM_SIZE="${SHM_SIZE:-512M}"
WORK="${WORK:-/tmp/invfs-plugin-smoke}"
SO="tools/codecpacks/$PACK.codecpack/lib$PACK.so"
CLI="tools/codecpacks/$PACK.codecpack/bin/$PACK"

need=$((WORKERS * 64))
have=$(df -m --output=avail /dev/shm 2>/dev/null | tail -1 | tr -d ' ' || echo 0)
if [ "${have:-0}" -lt "$need" ]; then
    echo "SKIP: $WORKERS workers need ${need} MiB of /dev/shm, this host has ${have} MiB."
    echo "      (run me inside a bigger tmpfs, e.g. unshare -rm + mount -t tmpfs)"
    exit 0
fi

rm -rf "$WORK"; mkdir -p "$WORK/mbr"
make -s plugin-so >/dev/null
[ -x "$CLI" ] || cc -std=c11 -O2 -Wall -Wextra -Werror -o "$CLI" \
    "tools/codecpacks/$PACK.codecpack/$PACK.c" -lz
[ -x bin/invf-plugin-host ] || make -s bin/invf-plugin-host >/dev/null

# ---- fixture -------------------------------------------------------------
case "$PACK" in
  rawdisk)
    python3 - "$WORK" <<'PY'
import os, sys, zlib, struct, random, uuid
d = sys.argv[1]; SEC = 512; nsec = 8192
img = bytearray(b"\xa5" * (nsec * SEC))
def text(n):
    s = b""; i = 0
    while len(s) < n:
        s += ("line %06d: the quick brown fox jumps over the lazy dog\n" % i).encode(); i += 1
    return s[:n]
parts = [("boot", 2048, 128, text(64 << 10)),
         ("rootfs", 2304, 256, bytes(random.Random(11).randrange(256) for _ in range(128 << 10))),
         ("data", 2816, 64, bytes(32 << 10))]
def guid(s): return uuid.UUID(s).bytes_le
LINUX_FS = "0fc63daf-8483-4772-8e79-3d69d8477de4"
entries = bytearray(128 * 128)
for i, (nm, start, cnt, payload) in enumerate(parts):
    e = entries[i * 128:(i + 1) * 128]
    e[0:16] = guid(LINUX_FS)
    e[16:32] = guid("11111111-2222-3333-4444-%012d" % (i + 1))
    struct.pack_into("<QQ", e, 32, start, start + cnt - 1)
    nm16 = nm.encode("utf-16-le"); e[56:56 + len(nm16)] = nm16
    entries[i * 128:(i + 1) * 128] = e
    img[start * SEC:(start + cnt) * SEC] = payload
ent_crc = zlib.crc32(bytes(entries)) & 0xFFFFFFFF
def header(cur, bak, el):
    h = bytearray(SEC); h[0:8] = b"EFI PART"
    struct.pack_into("<I", h, 8, 0x00010000); struct.pack_into("<I", h, 12, 92)
    struct.pack_into("<Q", h, 24, cur); struct.pack_into("<Q", h, 32, bak)
    struct.pack_into("<Q", h, 40, 2048); struct.pack_into("<Q", h, 48, nsec - 34)
    h[56:72] = guid("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee")
    struct.pack_into("<Q", h, 72, el)
    struct.pack_into("<I", h, 80, 128); struct.pack_into("<I", h, 84, 128)
    struct.pack_into("<I", h, 88, ent_crc)
    struct.pack_into("<I", h, 16, zlib.crc32(bytes(h[:92])) & 0xFFFFFFFF)
    return h
pmbr = bytearray(SEC)
pmbr[446:462] = bytes([0, 0, 2, 0, 0xEE, 0xFE, 0xFF, 0xFF]) + struct.pack("<II", 1, nsec - 1)
pmbr[510:512] = b"\x55\xAA"
img[0:SEC] = pmbr
img[SEC:2 * SEC] = header(1, nsec - 1, 2)
img[2 * SEC:34 * SEC] = entries
img[(nsec - 33) * SEC:(nsec - 1) * SEC] = entries
img[(nsec - 1) * SEC:nsec * SEC] = header(nsec - 1, 1, nsec - 33)
open(os.path.join(d, "img"), "wb").write(bytes(img))
PY
    ;;
  qcow2)
    command -v qemu-img >/dev/null || { echo "SKIP: qemu-img not installed"; exit 0; }
    qemu-img create -f qcow2 "$WORK/img" 4M >/dev/null
    qemu-io -c 'write -P 0x5a 0 64k' "$WORK/img" >/dev/null 2>&1
    ;;
  *) echo "no fixture builder for PACK=$PACK"; exit 1 ;;
esac
IMG="$WORK/img"

# ---- the client: exactly what invfs_codec_pack_cmd() does ----------------
cat > "$WORK/client.c" <<'EOF'
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "core/vol_plugin_client.h"

static int fails;
static void ck(int cond, const char *fmt, ...)
{
    va_list ap;
    if (cond) { printf("  ok    "); } else { printf("  FAIL  "); fails++; }
    va_start(ap, fmt); vprintf(fmt, ap); va_end(ap); printf("\n");
}

int main(int argc, char **argv)
{
    const char *pack = argv[1], *so = argv[2], *img = argv[3], *dir = argv[4];
    char tbl[512], rec[512], reb[512], mbr[512], mdir[512], idx[32], nm[256], line[1024];
    uint64_t est = 0;
    unsigned long long usize;
    FILE *f;
    int rc, n = 0;

    snprintf(tbl, sizeof tbl, "%s/table", dir);
    snprintf(rec, sizeof rec, "%s/recipe", dir);
    snprintf(reb, sizeof reb, "%s/rebuilt", dir);
    snprintf(mdir, sizeof mdir, "%s/mbr", dir);

    ck(invfs_plugin_pool_is_available(), "daemon control socket present");
    rc = invfs_plugin_pool_connect();
    ck(rc == 0, "client attached to the pool (rc=%d)", rc);
    if (rc != 0) return 1;

    rc = invfs_plugin_pool_container_estimate(pack, so, img, &est);
    ck(rc == 0 && est > 0, "estimate over IPC: rc=%d mbr_size=%llu", rc,
       (unsigned long long)est);

    rc = invfs_plugin_pool_container_cmd(pack, so, 1, img, NULL, tbl, NULL, NULL);
    ck(rc == 0, "ENUMERATE over IPC: rc=%d", rc);
    rc = invfs_plugin_pool_container_cmd(pack, so, 3, img, NULL, rec, NULL, NULL);
    ck(rc == 0, "STRIP over IPC: rc=%d", rc);

    f = fopen(tbl, "r");
    if (!f) { printf("  FAIL  cannot read the member table\n"); return 1; }
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%31s %255s %llu", idx, nm, &usize) != 3) continue;
        snprintf(mbr, sizeof mbr, "%s/mbr/%s", dir, idx);
        rc = invfs_plugin_pool_container_cmd(pack, so, 2, img, idx, mbr, NULL, NULL);
        ck(rc == 0, "EXTRACT idx=%s over IPC: rc=%d", idx, rc);
        n++;
    }
    fclose(f);
    ck(n > 0, "extracted %d member(s) through the daemon", n);

    rc = invfs_plugin_pool_container_cmd(pack, so, 4, NULL, NULL, reb, rec, mdir);
    ck(rc == 0, "REBUILD over IPC: rc=%d", rc);

    invfs_plugin_pool_disconnect();
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
EOF
cc -std=gnu11 -O1 -Wall -Isrc -o "$WORK/client" "$WORK/client.c" \
    build/obj/vol_plugin_client.o -lpthread

# ---- daemon --------------------------------------------------------------
./bin/invf-plugin-host -n "$WORKERS" > "$WORK/host.log" 2>&1 &
HOST_PID=$!
trap 'kill -TERM $HOST_PID 2>/dev/null || true; rm -rf "$WORK"' EXIT
for _ in $(seq 1 50); do
    [ -S /tmp/invfs_plugin_pool.sock ] && break
    sleep 0.1
done
cat "$WORK/host.log"

"$WORK/client" "$PACK" "$SO" "$IMG" "$WORK"
CLIENT_RC=$?

echo "== the rebuilt image must equal the original, bit for bit =="
if cmp -s "$IMG" "$WORK/rebuilt"; then
    echo "  ok    rebuild through the daemon is bit-exact ($(stat -c%s "$IMG") bytes)"
else
    echo "  FAIL  rebuild differs from the original"
    CLIENT_RC=1
fi
echo "== every member must equal what the CLI helper extracts =="
for m in "$WORK"/mbr/*; do
    i=$(basename "$m")
    "$CLI" extract "$IMG" "$i" "$WORK/cli-$i" >/dev/null
    if cmp -s "$m" "$WORK/cli-$i"; then
        echo "  ok    member $i: daemon output == CLI output"
    else
        echo "  FAIL  member $i differs from the CLI"
        CLIENT_RC=1
    fi
done

kill -TERM $HOST_PID 2>/dev/null || true
wait $HOST_PID 2>/dev/null || true
trap - EXIT
rm -rf "$WORK"
exit $CLIENT_RC
