import sys
try:
    with open(sys.argv[1], "rb") as f:
        sb = f.read(0x91)
except OSError:
    sys.exit(1)
sys.exit(0 if len(sb) >= 0x91 and sb[0x90] == 3 else 1)
