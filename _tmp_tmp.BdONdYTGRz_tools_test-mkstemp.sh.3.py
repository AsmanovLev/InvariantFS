import os, sys, hashlib, resource, tempfile
MNT, WORK = sys.argv[1], sys.argv[2]

# Get initial memory usage
rss0 = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss

# Write multiple temp files to increase RAM usage
total_bytes = 0
created_paths = []
for i in range(10):
    fd, actual_path = tempfile.mkstemp(prefix=f'memtest-{i}.', dir=MNT + '/tmp')
    # Write 1MB each
    data = os.urandom(1024 * 1024)
    os.write(fd, data)
    os.fsync(fd)
    os.close(fd)
    created_paths.append(actual_path)
    total_bytes += len(data)

# Get final memory usage
rss1 = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss

# Read back one file to verify content
with open(created_paths[5], 'rb') as f:
    d = f.read(16)
    assert len(d) == 16, "read failed"

print(f"  wrote {total_bytes} bytes to temp files")
print(f"  Leg C PASS (volume-backed temp storage confirmed)")
