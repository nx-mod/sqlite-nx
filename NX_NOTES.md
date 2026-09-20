# sqlite-nx

SQLite with a native Nintendo Switch VFS, for [wii-nx](https://github.com/nx-mod/wii-nx).

`source/nx-vfs.c` implements `sqlite3_vfs` directly on libnx's `fsFile*` API and registers itself as the
default from `sqlite3_os_init()`. It bypasses newlib's POSIX layer, which is what breaks on Horizon.

## Why it matters

wii-nx keeps its compiled shaders in SQLite databases. On stock SQLite there, every launch recompiled
every shader:

- Paths like `sdmc:/...` are not treated as absolute, and the default VFS then resolves them against a
  working directory Horizon does not have - the open fails outright.
- With that worked around, the first write still failed ("disk I/O error", then "malformed"), because the
  POSIX layer underneath does not behave as SQLite expects.

This VFS removes that layer, so the caches can persist.

## Behaviour

- Paths: `/dir/file.db` or `sdmc:/dir/file.db` on the SD card.
- Locking is in-process: any number of connections and threads may share a database (they share one
  file handle, since Horizon will not open a file for writing twice). Two processes cannot.
- Journal modes DELETE, TRUNCATE and MEMORY work; WAL does not (no shared memory). Temp storage is in
  memory (`SQLITE_TEMP_STORE=3`).
- Every commit on the SD card is slow (journal create + flush + delete): batch writes into
  transactions.

## Demo

`samples/switch-demo` builds `sqlite-nx-demo.nro`: 21 checks covering growth, VACUUM, two threads on
two cores, rollback, all three journal modes, temp tables, FTS4, read-only opens and missing files.
The report is also written to `sdmc:/switch/sqlite-nx-demo.log`. All 21 pass on hardware.

## Plan

1. Wire into wii-nx: build with `SQLITE_OS_OTHER=1` plus `nx-vfs.c`, and drop the workaround in Aurora.
2. Add a small CMake file for devkitPro builds (only a Makefile and `build.sh` today).
3. Journal mode stays `MEMORY` for the shader caches: fastest, and a lost cache only costs a recompile.

## Releases

Prebuilt packages are tagged `<upstream version>-nx-mod-v<n>`, the same convention across every nx-mod
library, so a project can pin one line per dependency.
