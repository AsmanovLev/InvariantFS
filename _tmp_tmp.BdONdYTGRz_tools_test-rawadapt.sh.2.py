import sys
open(sys.argv[1], 'wb').write(open(sys.argv[2], 'rb').read())
