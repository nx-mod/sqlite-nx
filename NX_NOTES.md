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

## Plan

1. Wire into wii-nx: build with `SQLITE_OS_OTHER=1` plus `nx-vfs.c`, and drop the workaround in Aurora.
2. Add a small CMake file for devkitPro builds (only a Makefile and `build.sh` today).
3. Journal mode stays `MEMORY` on Switch: WAL needs shared memory Horizon lacks.
