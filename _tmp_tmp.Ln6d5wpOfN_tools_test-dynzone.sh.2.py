import sys
open(sys.argv[1] + '/m.bin', 'wb').write(open(sys.argv[2], 'rb').read())
