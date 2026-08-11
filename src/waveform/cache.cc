/*
 * cache.cc
 * Waveform Seekbar plugin for Audacious
 *
 * See cache.h.
 */

#include "cache.h"

#include <mutex>
#include <string.h>
#include <stdio.h>

#include <sqlite3.h>

namespace WaveCache
{
static std::mutex s_mutex;
static sqlite3 * s_db = nullptr;

void open_db(const char * dir)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_db)
        return;

    char path[4096];
    snprintf(path, sizeof path, "%s/waveform-cache.db", dir);

    if (sqlite3_open(path, &s_db) != SQLITE_OK)
    {
        fprintf(stderr, "waveform: can't open cache db: %s\n", sqlite3_errmsg(s_db));
        sqlite3_close(s_db);
        s_db = nullptr;
        return;
    }

    char * err = nullptr;
    const char * query = "CREATE TABLE IF NOT EXISTS wave "
                          "(path TEXT PRIMARY KEY NOT NULL, channels INTEGER NOT NULL, data BLOB)";
    if (sqlite3_exec(s_db, query, nullptr, nullptr, &err) != SQLITE_OK)
    {
        fprintf(stderr, "waveform: cache schema error: %s\n", err);
        sqlite3_free(err);
    }
}

void close_db()
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_db)
    {
        sqlite3_close(s_db);
        s_db = nullptr;
    }
}

bool read(const char * key, WaveData & out)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_db)
        return false;

    sqlite3_stmt * stmt = nullptr;
    const char * query = "SELECT channels, data FROM wave WHERE path = ?1";
    if (sqlite3_prepare_v2(s_db, query, -1, &stmt, nullptr) != SQLITE_OK)
        return false;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);

    bool ok = false;
    if (sqlite3_step(stmt) == SQLITE_ROW)
    {
        int channels = sqlite3_column_int(stmt, 0);
        int bytes = sqlite3_column_bytes(stmt, 1);
        const void * blob = sqlite3_column_blob(stmt, 1);

        if (channels > 0 && bytes > 0 && blob)
        {
            out.channels = channels;
            out.data_len = bytes / sizeof(short);
            out.data = new short[out.data_len];
            memcpy(out.data, blob, out.data_len * sizeof(short));
            ok = true;
        }
    }

    sqlite3_finalize(stmt);
    return ok;
}

void write(const char * key, const short * data, size_t data_len, int channels)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_db)
        return;

    sqlite3_stmt * stmt = nullptr;
    const char * query = "INSERT OR REPLACE INTO wave (path, channels, data) VALUES (?1, ?2, ?3)";
    if (sqlite3_prepare_v2(s_db, query, -1, &stmt, nullptr) != SQLITE_OK)
        return;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, channels);
    sqlite3_bind_blob(stmt, 3, data, (int)(data_len * sizeof(short)), SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_DONE)
        fprintf(stderr, "waveform: cache write error: %s\n", sqlite3_errmsg(s_db));

    sqlite3_finalize(stmt);
}

void remove(const char * key)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    if (!s_db)
        return;

    sqlite3_stmt * stmt = nullptr;
    const char * query = "DELETE FROM wave WHERE path = ?1";
    if (sqlite3_prepare_v2(s_db, query, -1, &stmt, nullptr) != SQLITE_OK)
        return;

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}
} // namespace WaveCache
