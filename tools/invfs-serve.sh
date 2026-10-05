#!/bin/bash
# invfs-serve.sh -- least-invasive install server for ISO-based provisioning.
#
# Serves a directory to QEMU guests over the SLIRP host address (10.0.2.2).
# The guest boots a STOCK, checksum-verified ISO and fetches everything
# InvariantFS-specific from here (setup scripts + tool binaries), so the
# media stays pristine and all automation lives on the host, in git.
#
#   bash tools/invfs-serve.sh <serve-dir> [port]   # foreground; logs fetches
#
# Per-distro guest entry points (served as files, fetched by each ISO's own
# auto-run hook -- no custom media, no rebaked initramfs):
#   Arch     kernel cmdline: script=http://10.0.2.2:PORT/arch-setup.sh
#   Debian   kernel cmdline: auto=true preseed/url=http://10.0.2.2:PORT/preseed.cfg
#   Alpine   setup-alpine -f http://10.0.2.2:PORT/alpine-answerfile
#   Void     no hook -- TTY-push the curl line at the live shell prompt
#
# Bound to 127.0.0.1 + the SLIRP gateway only; killed with the harness.
set -euo pipefail

DIR=${1:?usage: invfs-serve.sh <serve-dir> [port]}
PORT=${2:-8000}
[ -d "$DIR" ] || { echo "no such dir: $DIR" >&2; exit 1; }

# Log every fetch with a timestamp: a failed run leaves the exact record of
# what the guest pulled, which is the install's audit trail.
cd "$DIR"
echo "invfs-serve: serving $DIR on 127.0.0.1:$PORT (guest reaches it as 10.0.2.2:$PORT)"
exec python3 -u -c '
import datetime, functools, http.server, sys
port = int(sys.argv[1])
class H(http.server.SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s %s\n" % (datetime.datetime.now().isoformat(timespec="seconds"), self.address_string(), fmt % args))
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()
http.server.ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
' "$PORT"
