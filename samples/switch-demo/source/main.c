// sqlite-nx demo: exercises the Switch VFS end to end and reports each check.
//
//   1. open/create a database on the SD card through an "sdmc:/" path
//   2. grow it well past its initial size (needs FsOpenMode_Append)
//   3. commit through a rollback journal that comes and goes (xAccess)
//   4. shrink it again with VACUUM (xTruncate)
//   5. write from two threads at once (libnx mutexes)
//   6. reopen and read everything back
#include <switch.h>
#include <sqlite3.h>
#include <stdio.h>

#define DB_PATH "sdmc:/switch/sqlite-nx-demo.db"
#define THREAD_ROWS 500

static int g_passed = 0;
static int g_failed = 0;

static void check(const char * what, int ok, sqlite3 * db) {
    if (ok) {
        g_passed++;
        printf("  [PASS] %s\n", what);
    } else {
        g_failed++;
        printf("  [FAIL] %s: %s\n", what, db ? sqlite3_errmsg(db) : "(no database)");
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

static sqlite3_int64 file_size(void) {
    FILE * f = fopen(DB_PATH, "rb");
    if (!f) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    return size;
}

// Each thread uses its own connection, as SQLite recommends.
static void writer(void * arg) {
    const int id = (int) (intptr_t) arg;
    sqlite3 * db = NULL;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) {
        sqlite3_close(db);
        return;
    }
    sqlite3_busy_timeout(db, 10000);
    sqlite3_exec(db, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL);
    for (int i = 0; i < THREAD_ROWS; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO threads(writer, n) VALUES(%d, %d)", id, i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
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

    // ~1 MiB of rows: the file has to grow many times over.
    int ok = sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) == SQLITE_OK;
    for (int i = 0; ok && i < 2000; i++) {
        ok = sqlite3_exec(db,
                          "INSERT INTO rows(payload) VALUES(printf('%.500c', 'x'))",
                          NULL, NULL, NULL) == SQLITE_OK;
    }
    ok = ok && sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) == SQLITE_OK;
    check("insert 2000 rows in one transaction", ok, db);
    const sqlite3_int64 grown = file_size();
    printf("         database is now %lld bytes\n", (long long) grown);
    check("file grew past 1 MiB", grown > 1024 * 1024, db);

    check("delete half and VACUUM",
          sqlite3_exec(db, "DELETE FROM rows WHERE id % 2 = 0; VACUUM;", NULL, NULL, NULL) == SQLITE_OK, db);
    const sqlite3_int64 shrunk = file_size();
    printf("         database is now %lld bytes\n", (long long) shrunk);
    check("file shrank after VACUUM", shrunk > 0 && shrunk < grown, db);

    sqlite3_close(db);
    db = NULL;

    // Two threads, two connections, one database.
    Thread threads[2];
    for (int t = 0; t < 2; t++) {
        threadCreate(&threads[t], writer, (void *) (intptr_t) (t + 1), NULL, 0x20000, 0x2C, -2);
        threadStart(&threads[t]);
    }
    for (int t = 0; t < 2; t++) {
        threadWaitForExit(&threads[t]);
        threadClose(&threads[t]);
    }

    check("reopen", sqlite3_open(DB_PATH, &db) == SQLITE_OK, db);
    check("1000 rows survive the round trip", scalar(db, "SELECT count(*) FROM rows") == 1000, db);
    check("both threads' rows landed",
          scalar(db, "SELECT count(*) FROM threads") == 2 * THREAD_ROWS, db);
    check("integrity_check", scalar(db, "SELECT count(*) FROM pragma_integrity_check WHERE integrity_check='ok'") == 1, db);
    sqlite3_close(db);
}

int main(int argc, char * argv[]) {
    consoleInit(NULL);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    run();
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
