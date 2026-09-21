# sqlite-nx

SQLite with a native Nintendo Switch VFS: `sqlite3_vfs` implemented directly on libnx's `fsFile*` API,
bypassing newlib's POSIX layer.

This is the upstream SQLite tree plus one file, `source/nx-vfs.c`, which registers itself as the default
VFS from `sqlite3_os_init()`. Upstream's own README is [docs/SQLITE_README.md](docs/SQLITE_README.md).

## Why

On stock SQLite, a database on the SD card cannot be opened at all: paths like `sdmc:/dir/file.db` are
not treated as absolute, and the default VFS resolves them against a working directory Horizon does not
have. Work around that and the first write still fails - "disk I/O error", then "database disk image is
malformed" - because the POSIX layer underneath does not behave the way SQLite expects.

This replaces that layer rather than patching around it, so a database on the SD card behaves normally.

## Using it

Build SQLite with its OS layer disabled and add one file:

```cmake
target_compile_definitions(sqlite PRIVATE SQLITE_OS_OTHER=1 SQLITE_TEMP_STORE=3)
target_sources(sqlite PRIVATE ${SQLITE_NX_DIR}/source/nx-vfs.c)
```

Then open databases as `/dir/file.db` or `sdmc:/dir/file.db`. Nothing else in your code changes.

## What works, and what does not

- Any number of connections and threads may share a database. They share one file handle, because
  Horizon will not open a file for writing twice, and locking is **in-process**: two processes cannot
  share a database.
- Journal modes DELETE, TRUNCATE and MEMORY work. WAL does not - it needs shared memory.
- Temp storage is in memory (`SQLITE_TEMP_STORE=3`).
- Every commit to the SD card is slow: journal create, flush, delete. Batch writes into transactions.

## Demo

```sh
cd samples/switch-demo && make       # sqlite-nx-demo.nro
```

21 checks: growth, VACUUM, two threads on two cores, rollback, all three journal modes, temp tables,
FTS4, read-only opens, missing files. The report is written to `sdmc:/switch/sqlite-nx-demo.log`.
All 21 pass on hardware.

## Releases

Prebuilt packages are tagged `<upstream version>-nx-mod-v<n>`, so a project can pin one line per
dependency.

## License

SQLite is public domain; `source/nx-vfs.c` is released the same way.
