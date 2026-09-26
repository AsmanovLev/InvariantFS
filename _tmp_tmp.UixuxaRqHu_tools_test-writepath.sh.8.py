import os, sys
data = open(sys.argv[1] + '/victim.bin', 'rb').read()
if len(data) != 2 * 1024 * 1024:
    sys.exit("FAIL: victim.bin size %d after crash" % len(data))
if data[0:16] != b'FLUSHED-REGION--':
    sys.exit("FAIL: msynced+fsynced region lost after kill -9")
print("  post-crash: fsynced region intact; unsynced region %s"
      % ("also landed (kernel wrote it back before the kill)"
         if data[1048576:1048592] == b'NEVER-MSYNCED-XX' else "absent (legal)"))
