import io, sys, tarfile
out = io.BytesIO()
with tarfile.open(fileobj=out, mode="w") as tf:
    for name in ("m1.txt", "m2.txt"):
        data = ("the quick brown fox jumps over lazy dogs\n"
                "int static return while for struct char void\n") * 400
        ti = tarfile.TarInfo(name)
        ti.size = len(data)
        tf.addfile(ti, io.BytesIO(data.encode()))
open(sys.argv[1], "wb").write(out.getvalue())
