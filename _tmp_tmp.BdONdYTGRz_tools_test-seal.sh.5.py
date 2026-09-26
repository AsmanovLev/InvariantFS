import os
d = "/dev/shm/wp20seal/origc"
data = open(os.path.join(d, "note.txt")).read()
open(os.path.join(d, "note.txt"), "w").write(data + "the second edition\n" * 3000)
