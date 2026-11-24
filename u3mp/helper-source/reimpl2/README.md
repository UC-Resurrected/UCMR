# Helper functions written again from descriptions

Each folder holds the new C++ for one group of helper functions. Every file here was written from the group's
description in [`../spec/`](../spec/) only (never from the old code), and its group passed the old-versus-new test at
-O1 and -O2 before it was added here. Since helper v7.213 all of them are merged into `u3x_fixes.cpp` (each group in
its own `namespace r2_<group>` between `r2 begin` / `r2 end` markers) and the group tests passed again on the merged
file; these copies are the parts as tested.

`CONVENTIONS.h` holds the shared small tools the files use (single-precision float steps, pointer test, game call).
Five groups that talk to the online service are not in this folder.
