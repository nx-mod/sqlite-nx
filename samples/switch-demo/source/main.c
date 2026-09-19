// sqlite-nx demo: exercises the Switch VFS end to end and reports each check.
//
//   1. open/create a database on the SD card through an "sdmc:/" path
//   2. grow it well past its initial size (needs FsOpenMode_Append)
//   3. commit through a rollback journal that comes and goes (xAccess)
//   4. shrink it again with VACUUM (xTruncate)
//   5. write from two threads at once (libnx mutexes)
//   6. reopen and read everything back
//   7. roll a transaction back (reads the journal back in)
//   8. TRUNCATE and MEMORY journal modes, temp tables, FTS4
//   9. read-only opens, and a missing file without CREATE
//
// The report also goes to sdmc:/switch/sqlite-nx-demo.log.
#include <switch.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdio.h>

#define DB_PATH "sdmc:/switch/sqlite-nx-demo.db"
#define THREAD_ROWS 500

static int g_passed = 0;
static int g_failed = 0;
static FILE * g_log = NULL;   // sdmc:/switch/sqlite-nx-demo.log: the same report, readable off-console

static void check(const char * what, int ok, sqlite3 * db) {
    char line[256];
    if (ok) {
        g_passed++;
        snprintf(line, sizeof(line), "  [PASS] %s\n", what);
    } else {
        g_failed++;
        snprintf(line, sizeof(line), "  [FAIL] %s: %s (code %d)\n", what,
                 db ? sqlite3_errmsg(db) : "(no database)", db ? sqlite3_extended_errcode(db) : -1);
    }
    printf("%s", line);
    if (g_log) {
        fputs(line, g_log);
        fflush(g_log);
    }
    consoleUpdate(NULL);
}

static sqlite3_int64 scalar(sqlite3 * db, const char * sql) {
    sqlite3_stmt * stmt = NULL;
    sqlite3_int64 value = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value;
}

static void report(const char * fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    printf("%s", line);
    if (g_log) {
        fputs(line, g_log);
        fflush(g_log);
    }
}

// Size on disk as the VFS reports it (Horizon may refuse a second open of a
// file SQLite holds for writing, so no fopen here).
static sqlite3_int64 file_size(sqlite3 * db) {
    sqlite3_file * file = NULL;
    sqlite3_int64 size = -1;
    if (sqlite3_file_control(db, "main", SQLITE_FCNTL_FILE_POINTER, &file) == SQLITE_OK && file && file->pMethods) {
        file->pMethods->xFileSize(file, &size);
    }
    return size;
}

typedef struct {
    int id;
    int inserted;
    char error[128];
} WriterResult;

// Each thread uses its own connection, as SQLite recommends.
static void writer(void * arg) {
    WriterResult * result = (WriterResult *) arg;
    sqlite3 * db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        snprintf(result->error, sizeof(result->error), "open: %s", db ? sqlite3_errmsg(db) : "(no database)");
        sqlite3_close(db);
        return;
    }
    sqlite3_busy_timeout(db, 10000);
    sqlite3_exec(db, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL);
    for (int i = 0; i < THREAD_ROWS; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO threads(writer, n) VALUES(%d, %d)", result->id, i);
        if (sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK) {
            result->inserted++;
        } else if (!result->error[0]) {
            snprintf(result->error, sizeof(result->error), "row %d: %s (code %d)", i, sqlite3_errmsg(db),
                     sqlite3_extended_errcode(db));
        }
    }
    sqlite3_close(db);
}

static void run(void) {
    sqlite3 * db = NULL;
    remove(DB_PATH);

    printf("sqlite-nx %s demo\n\n", sqlite3_libversion());

    check("open " DB_PATH, sqlite3_open(DB_PATH, &db) == SQLITE_OK, db);
    if (!db) {
        return;
    }
    // DELETE mode creates and removes the journal on every commit, so both
    // xAccess answers (present / absent) get exercised.
    check("journal_mode=DELETE", sqlite3_exec(db, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL) == SQLITE_OK, db);
    check("create tables",
          sqlite3_exec(db,
                       "CREATE TABLE rows(id INTEGER PRIMARY KEY, payload TEXT);"
                       "CREATE TABLE threads(writer INTEGER, n INTEGER);",
                       NULL, NULL, NULL) == SQLITE_OK,
          db);

    // ~1 MB of rows: the file has to grow many times over.
    int ok = sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK;
    for (int i = 0; ok && i < 2000; i++) {
        ok = sqlite3_exec(db,
                          "INSERT INTO rows(payload) VALUES(printf('%.500c', 'x'))",
                          NULL, NULL, NULL) == SQLITE_OK;
    }
    ok = ok && sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
    check("insert 2000 rows in one transaction", ok, db);
    const sqlite3_int64 grown = file_size(db);
    report("         database is now %lld bytes\n", (long long) grown);
    check("file grew past 1 MB", grown > 1000 * 1000, db);

    check("delete half and VACUUM",
          sqlite3_exec(db, "DELETE FROM rows WHERE id % 2 = 0; VACUUM;", NULL, NULL, NULL) == SQLITE_OK, db);
    const sqlite3_int64 shrunk = file_size(db);
    report("         database is now %lld bytes\n", (long long) shrunk);
    check("file shrank after VACUUM", shrunk > 0 && shrunk < grown, db);

    sqlite3_close(db);
    db = NULL;

    // Two threads, two connections, one database - on cores 1 and 2, so they
    // really do run at the same time and contend for the lock.
    Thread threads[2];
    WriterResult results[2] = {{.id = 1}, {.id = 2}};
    for (int t = 0; t < 2; t++) {
        threadCreate(&threads[t], writer, &results[t], NULL, 0x20000, 0x2C, t + 1);
        threadStart(&threads[t]);
    }
    for (int t = 0; t < 2; t++) {
        threadWaitForExit(&threads[t]);
        threadClose(&threads[t]);
        report("         writer %d inserted %d/%d%s%s\n", results[t].id, results[t].inserted, THREAD_ROWS,
             results[t].error[0] ? ", first error: " : "", results[t].error);
    }

    check("reopen", sqlite3_open(DB_PATH, &db) == SQLITE_OK, db);
    check("1000 rows survive the round trip", scalar(db, "SELECT count(*) FROM rows") == 1000, db);
    check("both threads' rows landed",
          scalar(db, "SELECT count(*) FROM threads") == 2 * THREAD_ROWS, db);
    check("integrity_check", scalar(db, "SELECT count(*) FROM pragma_integrity_check WHERE integrity_check='ok'") == 1, db);

    sqlite3_exec(db, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL);
    check("rollback restores deleted rows",
          sqlite3_exec(db, "BEGIN; DELETE FROM rows; ROLLBACK;", NULL, NULL, NULL) == SQLITE_OK &&
              scalar(db, "SELECT count(*) FROM rows") == 1000,
          db);
    check("journal_mode=TRUNCATE commit",
          sqlite3_exec(db, "PRAGMA journal_mode=TRUNCATE; INSERT INTO rows(payload) VALUES('truncate');", NULL, NULL,
                       NULL) == SQLITE_OK,
          db);
    check("journal_mode=MEMORY commit",
          sqlite3_exec(db, "PRAGMA journal_mode=MEMORY; INSERT INTO rows(payload) VALUES('memory');", NULL, NULL,
                       NULL) == SQLITE_OK,
          db);
    check("temp table (in memory)",
          sqlite3_exec(db, "CREATE TEMP TABLE scratch(x); INSERT INTO scratch VALUES(1),(2),(3);", NULL, NULL, NULL) ==
                  SQLITE_OK &&
              scalar(db, "SELECT sum(x) FROM scratch") == 6,
          db);
    check("FTS4 full-text search",
          sqlite3_exec(db,
                       "CREATE VIRTUAL TABLE docs USING fts4(body);"
                       "INSERT INTO docs VALUES('mario kart wii'),('new super mario bros'),('zelda');",
                       NULL, NULL, NULL) == SQLITE_OK &&
              scalar(db, "SELECT count(*) FROM docs WHERE docs MATCH 'mario'") == 2,
          db);
    sqlite3_close(db);
    db = NULL;

    check("read-only open", sqlite3_open_v2(DB_PATH, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK, db);
    check("read-only reads", scalar(db, "SELECT count(*) FROM rows") == 1002, db);
    check("read-only refuses writes",
          sqlite3_exec(db, "INSERT INTO rows(payload) VALUES('nope')", NULL, NULL, NULL) == SQLITE_READONLY, db);
    sqlite3_close(db);
    db = NULL;

    const int missing = sqlite3_open_v2("sdmc:/switch/sqlite-nx-missing.db", &db, SQLITE_OPEN_READWRITE, NULL);
    check("missing file without CREATE is CANTOPEN", missing == SQLITE_CANTOPEN, NULL);
    sqlite3_close(db);
}

int main(int argc, char * argv[]) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    g_log = fopen("sdmc:/switch/sqlite-nx-demo.log", "w");
    run();
    if (g_log) {
        fprintf(g_log, "%d passed, %d failed.\n", g_passed, g_failed);
        fclose(g_log);
    }
    printf("\n%d passed, %d failed.\n\nPress + to exit.\n", g_passed, g_failed);
    consoleUpdate(NULL);

    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) {
            break;
        }
        consoleUpdate(NULL);
    }
    consoleExit(NULL);
    return 0;
}
