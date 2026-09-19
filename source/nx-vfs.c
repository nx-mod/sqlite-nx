#include "sqlite3.h"
#include <switch.h>
#include <string.h>
#include <stdio.h>

// SQLite VFS on libnx's FsFileSystem, bypassing newlib's POSIX layer.
//
// - Paths are SD card paths: "/dir/file.db", or "sdmc:/dir/file.db".
// - No file locking (Horizon has none), so one process per database.
// - No temp files: build with SQLITE_TEMP_STORE=3 so SQLite keeps them in memory.
// - No shared memory, so no WAL: use journal_mode=MEMORY, DELETE or TRUNCATE.
// - No dynamic libraries.

// FsFileSystem used to interact with files on the SD card
static FsFileSystem fs;

// libnx takes paths relative to the filesystem root; accept the "sdmc:"
// device prefix newlib users naturally pass.
static const char * nxPath(const char * path) {
    if (strncmp(path, "sdmc:", 5) == 0) {
        return path + 5;
    }
    return path;
}

// Size of write buffer (in bytes)
#define SQLITE_NXVFS_BUFFERSZ 8192

// sqlite3_file * actually points to this structure
typedef struct nxFile nxFile;
struct nxFile {
    sqlite3_file base;          // Base class
    FsFile file;                // NX (Horizon) file object
    char * buf;                 // Buffer for writes
    int bufSize;                // Number of bytes in buffer
    sqlite3_int64 bufOffset;    // Offset of bytes in buffer from buf[0]
};

// Close a file
static int nxFlushBuffer(nxFile * file);

static int nxClose(sqlite3_file * pFile) {
    nxFile * file = (nxFile *) pFile;
    int rc = nxFlushBuffer(file);   // Buffered writes must reach the file
    fsFileClose(&file->file);
    sqlite3_free(file->buf);
    file->buf = NULL;
    return rc;
}

// Read data from a file
static int nxRead(sqlite3_file * pFile, void * buf, int bytes, sqlite_int64 offset) {
    // Bytes read and result code
    u64 read = 0;
    Result rc;

    // Read from file
    nxFile * file = (nxFile *) pFile;
    rc = fsFileRead(&file->file, offset, buf, bytes, FsReadOption_None, &read);

    // Return IO error if result isn't good
    if (R_FAILED(rc)) {
        return SQLITE_IOERR_READ;
    }

    // Check if we read the right amount of bytes
    if (read == bytes) {
        return SQLITE_OK;

    // Zero-pad the remaining buffer if not enough bytes were read
    }

    // Short read: SQLite requires the rest of the buffer zeroed
    memset(&((char *) buf)[read], 0, bytes - read);
    return SQLITE_IOERR_SHORT_READ;
}

// Write to a file (and flush immediately)
static int nxDirectWrite(nxFile * file, const void * buf, int bytes, sqlite_int64 offset) {
    Result rc = fsFileWrite(&file->file, offset, buf, bytes, FsWriteOption_Flush);

    // Return IO error if result is not good
    if (R_FAILED(rc)) {
        return SQLITE_IOERR_WRITE;
    }

    return SQLITE_OK;
}

// Flush file's buffer to disk (no-op if buffer is empty)
// Keeps the buffer allocated: clearing the pointer here used to make the
// next buffered write copy through NULL, and leaked the buffer.
static int nxFlushBuffer(nxFile * file) {
    int rc = SQLITE_OK;
    if (file->buf && file->bufSize > 0) {
        rc = nxDirectWrite(file, file->buf, file->bufSize, file->bufOffset);
        file->bufSize = 0;
    }
    return rc;
}

// Write to a file (without flushing)
static int nxWrite(sqlite3_file * pFile, const void * buf, int bytes, sqlite_int64 offset) {
    nxFile * file = (nxFile *) pFile;

    // If the buffer exists
    if (file->buf) {
        char * buf2 = (char *) buf;         // Pointer to remaining data in write buffer
        int bytes2 = bytes;                 // Remaining number of bytes in write buffer
        sqlite3_int64 offset2 = offset;     // File offset to write to

        // While there's still bytes to write
        while (bytes2 > 0) {
            int copy;       // Number of bytes to copy into file buffer

            // If the buffer is full or not being used - flush the buffer
            if (file->bufSize == SQLITE_NXVFS_BUFFERSZ || file->bufOffset + file->bufSize != offset2) {
                int rc = nxFlushBuffer(file);
                if (rc != SQLITE_OK) {
                    return rc;
                }
            }
            file->bufOffset = offset2 - file->bufSize;

            // Copy as much data as possible into the buffer
            copy = SQLITE_NXVFS_BUFFERSZ - file->bufSize;
            if (copy > bytes2) {
                copy = bytes2;
            }
            memcpy(&file->buf[file->bufSize], buf2, copy);
            file->bufSize += copy;

            // Update variables
            bytes2 -= copy;
            offset2 += copy;
            buf2 += copy;
        }

    // Otherwise if there's no buffer just write to file
    } else {
        return nxDirectWrite(file, buf, bytes, offset);
    }

    return SQLITE_OK;
}

static int nxTruncate(sqlite3_file * pFile, sqlite_int64 size) {
    nxFile * file = (nxFile *) pFile;
    int tmp = nxFlushBuffer(file);
    if (tmp != SQLITE_OK) {
        return tmp;
    }
    Result rc = fsFileSetSize(&file->file, size);
    return (R_SUCCEEDED(rc) ? SQLITE_OK : SQLITE_IOERR_TRUNCATE);
}

// Sync contents of file to the disk
static int nxSync(sqlite3_file * pFile, int flags) {
    nxFile * file = (nxFile *) pFile;

    // Flush buffer to disk
    int tmp = nxFlushBuffer(file);
    if (tmp != SQLITE_OK) {
        return tmp;
    }

    // Call system to flush it's cache
    Result rc = fsFileFlush(&file->file);
    return (R_SUCCEEDED(rc) ? SQLITE_OK : SQLITE_IOERR_FSYNC);
}

// Get the size of the file and write to pointer
static int nxFileSize(sqlite3_file * pFile, sqlite_int64 * size) {
    nxFile * file = (nxFile *) pFile;

    // Flush buffer to disk first
    int tmp = nxFlushBuffer(file);
    if (tmp != SQLITE_OK) {
        return tmp;
    }

    // Query using system call
    s64 sz;
    Result rc = fsFileGetSize(&file->file, &sz);
    if (R_FAILED(rc)) {
        return SQLITE_IOERR_FSTAT;
    }
    *(size) = sz;
    return SQLITE_OK;
}

// All locking functions do nothing
static int nxLock(sqlite3_file * pFile, int lock) {
    return SQLITE_OK;
}
static int nxUnlock(sqlite3_file * pFile, int lock) {
    return SQLITE_OK;
}
static int nxCheckReservedLock(sqlite3_file * pFile, int * pResOut) {
    *pResOut = 0;
    return SQLITE_OK;
}

// File control also does nothing
static int nxFileControl(sqlite3_file * pFile, int op, void * arg) {
    return SQLITE_NOTFOUND;
}

// Don't return any info about device
static int nxSectorSize(sqlite3_file * pFile) {
    return 0;
}
static int nxDeviceCharacteristics(sqlite3_file * pFile) {
    return 0;
}

// Open a file
static int nxOpen(sqlite3_vfs * vfs, const char * path, sqlite3_file * pFile, int flags, int * outFlags) {
    // Set file's IO methods to the ones above
    static const sqlite3_io_methods nxIO = {
        1,                          // iVersion
        nxClose,                    // xClose
        nxRead,                     // xRead
        nxWrite,                    // xWrite
        nxTruncate,                 // xTruncate
        nxSync,                     // xSync
        nxFileSize,                 // xFileSize
        nxLock,                     // xLock
        nxUnlock,                   // xUnlock
        nxCheckReservedLock,        // xCheckReservedLock
        nxFileControl,              // xFileControl
        nxSectorSize,               // xSectorSize
        nxDeviceCharacteristics     // xDeviceCharacteristics
    };

    nxFile * file = (nxFile *) pFile;
    Result rc;
    char * tmpBuf = NULL;           // Temporary pointer to potential file buffer

    // Don't support temp files
    if (path == NULL) {
        return SQLITE_IOERR;
    }

    // Create file buffer if it's a journal file
    if (flags & SQLITE_OPEN_MAIN_JOURNAL) {
        tmpBuf = (char *) sqlite3_malloc(SQLITE_NXVFS_BUFFERSZ);
        if (!tmpBuf) {
            return SQLITE_NOMEM;
        }
    }

    path = nxPath(path);

    // Create file if flag is set (fails harmlessly if it already exists)
    if (flags & SQLITE_OPEN_CREATE) {
        fsFsCreateFile(&fs, path, 0, 0);
    }

    // Choose mode based on flags
    u32 mode = 0;
    if (flags & SQLITE_OPEN_READONLY) {
        mode |= FsOpenMode_Read;
    } else if (flags & SQLITE_OPEN_READWRITE) {
        // Append lets writes extend the file; without it Horizon rejects any
        // write past the current end, so a database could never grow.
        mode |= FsOpenMode_Read | FsOpenMode_Write | FsOpenMode_Append;
    }

    // Allocate memory for file object and open
    memset(pFile, 0, sizeof(nxFile));
    rc = fsFsOpenFile(&fs, path, mode, &file->file);
    if (R_FAILED(rc)) {
        file->base.pMethods = NULL;     // Prevents nxClose being called
        sqlite3_free(tmpBuf);
        return SQLITE_CANTOPEN;
    }
    file->buf = tmpBuf;

    // Set output flags
    if (outFlags) {
        *(outFlags) = flags;
    }
    file->base.pMethods = &nxIO;
    return SQLITE_OK;
}

// Delete the given file
static int nxDelete(sqlite3_vfs * vfs, const char * path, int sync) {
    Result rc = fsFsDeleteFile(&fs, nxPath(path));

    // Commit changes if flag set
    if (R_SUCCEEDED(rc) && sync) {
        fsFsCommit(&fs);
    }

    return (R_SUCCEEDED(rc) ? SQLITE_OK : SQLITE_IOERR_DELETE);
}

// Check if the file exists
// "Does it exist?" is a question, not an error: SQLite asks it about journal
// files that are normally absent, and reporting absence as SQLITE_IOERR turned
// ordinary opens into "disk I/O error". Readable/writable are assumed for any
// file that exists.
static int nxAccess(sqlite3_vfs * vfs, const char * path, int flags, int * out) {
    FsDirEntryType type = FsDirEntryType_Dir;
    Result rc = fsFsGetEntryType(&fs, nxPath(path), &type);
    *out = (R_SUCCEEDED(rc) && type == FsDirEntryType_File) ? 1 : 0;
    return SQLITE_OK;
}

// Simply returns the given path (should return full path though)
// Paths are already absolute on the SD card; copy with the terminator, and
// never past either buffer (the old version copied outBytes from a shorter path).
static int nxFullPathname(sqlite3_vfs * vfs, const char * path, int outBytes, char * outPath) {
    int num = (int) strlen(path);
    if (num + 1 > outBytes) {
        return SQLITE_CANTOPEN;
    }
    memcpy(outPath, path, num + 1);
    return SQLITE_OK;
}

// All dynamic library related functions do nothing due to no support
static void * nxDlOpen(sqlite3_vfs * vfs, const char * path){
  return NULL;
}
static void nxDlError(sqlite3_vfs * vfs, int bytes, char * err){
  sqlite3_snprintf(bytes, err, "Loadable extensions are not supported");
  err[bytes-1] = '\0';
}
static void (*nxDlSym(sqlite3_vfs * vfs, void * handle, const char * z))(void){
  return NULL;
}
static void nxDlClose(sqlite3_vfs * vfs, void * handle){
  return;
}

// Fill the provided buffer with pseudo-random bytes
static int nxRandomness(sqlite3_vfs * vfs, int bytes, char * buf) {
    randomGet((void *) buf, bytes);
    return SQLITE_OK;
}

// Sleep for the given number of microseconds
static int nxSleep(sqlite3_vfs * vfs, int mSecs) {
    svcSleepThread(mSecs * 1000);
    return mSecs;
}

// Returns the current time as UTC in Julian days
static int nxCurrentTime(sqlite3_vfs * vfs, double * time) {
    u64 ts;
    Result rc = timeGetCurrentTime(TimeType_Default, &ts);
    if (R_FAILED(rc)) {
        return SQLITE_ERROR;
    }

    *(time) = ts/86400.0 + 2440587.5;
    return SQLITE_OK;
}

// Mutexes on libnx. SQLITE_OS_OTHER ships only no-op mutexes, so without
// these a thread-safe build would not actually be thread-safe. Every mutex is
// recursive (RMutex), which also satisfies SQLITE_MUTEX_FAST.
struct sqlite3_mutex {
    RMutex lock;
};

// Static mutex ids run from 2 (STATIC_MAIN, named STATIC_MASTER before 3.33)
// through SQLITE_MUTEX_STATIC_VFS3.
#define NX_STATIC_MUTEX_FIRST 2
#define NX_STATIC_MUTEX_COUNT (SQLITE_MUTEX_STATIC_VFS3 - NX_STATIC_MUTEX_FIRST + 1)
static sqlite3_mutex nxStaticMutexes[NX_STATIC_MUTEX_COUNT];

static int nxMutexInit(void) {
    for (int i = 0; i < NX_STATIC_MUTEX_COUNT; i++) {
        rmutexInit(&nxStaticMutexes[i].lock);
    }
    return SQLITE_OK;
}
static int nxMutexEnd(void) {
    return SQLITE_OK;
}
static sqlite3_mutex * nxMutexAlloc(int id) {
    if (id == SQLITE_MUTEX_FAST || id == SQLITE_MUTEX_RECURSIVE) {
        sqlite3_mutex * mutex = (sqlite3_mutex *) sqlite3_malloc(sizeof(sqlite3_mutex));
        if (mutex) {
            rmutexInit(&mutex->lock);
        }
        return mutex;
    }
    if (id >= NX_STATIC_MUTEX_FIRST && id < NX_STATIC_MUTEX_FIRST + NX_STATIC_MUTEX_COUNT) {
        return &nxStaticMutexes[id - NX_STATIC_MUTEX_FIRST];
    }
    return NULL;
}
static void nxMutexFree(sqlite3_mutex * mutex) {
    sqlite3_free(mutex);
}
static void nxMutexEnter(sqlite3_mutex * mutex) {
    rmutexLock(&mutex->lock);
}
static int nxMutexTry(sqlite3_mutex * mutex) {
    return rmutexTryLock(&mutex->lock) ? SQLITE_OK : SQLITE_BUSY;
}
static void nxMutexLeave(sqlite3_mutex * mutex) {
    rmutexUnlock(&mutex->lock);
}

// Installed before main: sqlite3_config() must run before SQLite initializes,
// and sqlite3_os_init() is already too late for it.
__attribute__((constructor)) static void nxInstallMutexes(void) {
    static const sqlite3_mutex_methods methods = {
        nxMutexInit, nxMutexEnd, nxMutexAlloc, nxMutexFree,
        nxMutexEnter, nxMutexTry, nxMutexLeave, NULL, NULL,
    };
    sqlite3_config(SQLITE_CONFIG_MUTEX, &methods);
}

// Returns a pointer to this VFS so it can be used
sqlite3_vfs * sqlite3_nxvfs() {
    static sqlite3_vfs nxvfs = {
        1,                            // iVersion
        sizeof(nxFile),               // szOsFile
        FS_MAX_PATH,                  // mxPathname
        0,                            // pNext
        "nx",                         // zName
        0,                            // pAppData
        nxOpen,                       // xOpen
        nxDelete,                     // xDelete
        nxAccess,                     // xAccess
        nxFullPathname,               // xFullPathname
        nxDlOpen,                     // xDlOpen
        nxDlError,                    // xDlError
        nxDlSym,                      // xDlSym
        nxDlClose,                    // xDlClose
        nxRandomness,                 // xRandomness
        nxSleep,                      // xSleep
        nxCurrentTime,                // xCurrentTime
    };
    return &nxvfs;
}

// Opens the FsFileSystem and registers the VFS
SQLITE_API int sqlite3_os_init() {
    Result rc = fsOpenSdCardFileSystem(&fs);
    if (R_FAILED(rc)) {
        return SQLITE_ERROR;
    }
    sqlite3_vfs_register(sqlite3_nxvfs(), 1);
    return SQLITE_OK;
}

// Closes the FsFileSystem
SQLITE_API int sqlite3_os_end() {
    fsFsClose(&fs);
    return SQLITE_OK;
}