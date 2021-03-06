/*
 * melo_file_db.c: Database managment for File module
 *
 * Copyright (C) 2016 Alexandre Dilly <dillya@sparod.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#include <sqlite3.h>

#include "melo_file_db.h"

#define MELO_FILE_DB_VERSION 6
#define MELO_FILE_DB_VERSION_STR "6"

/* Table creation */
#define MELO_FILE_DB_CREATE \
  "CREATE TABLE song (" \
  "        'title'         TEXT," \
  "        'artist_id'     INTEGER," \
  "        'album_id'      INTEGER," \
  "        'genre_id'      INTEGER," \
  "        'date'          INTEGER," \
  "        'track'         INTEGER," \
  "        'tracks'        INTEGER," \
  "        'cover'         TEXT," \
  "        'file'          TEXT," \
  "        'path_id'       INTEGER," \
  "        'timestamp'     INTEGER" \
  ");" \
  "CREATE TABLE artist (" \
  "        'artist'        TEXT NOT NULL UNIQUE," \
  "        'cover'         TEXT" \
  ");" \
  "CREATE TABLE album (" \
  "        'album'         TEXT NOT NULL UNIQUE," \
  "        'cover'         TEXT" \
  ");" \
  "CREATE TABLE genre (" \
  "        'genre'         TEXT NOT NULL UNIQUE," \
  "        'cover'         TEXT" \
  ");" \
  "CREATE TABLE path (" \
  "        'path'          TEXT NOT NULL UNIQUE" \
  ");" \
  "CREATE VIRTUAL TABLE song_fts USING FTS4(file,title);" \
  "CREATE VIRTUAL TABLE artist_fts USING FTS4(artist);" \
  "CREATE VIRTUAL TABLE album_fts USING FTS4(album);" \
  "CREATE VIRTUAL TABLE genre_fts USING FTS4(genre);" \
  "PRAGMA user_version = " MELO_FILE_DB_VERSION_STR ";"

/* Get database version */
#define MELO_FILE_DB_GET_VERSION "PRAGMA user_version;"

/* Clean database */
#define MELO_FILE_DB_CLEAN \
  "DROP TABLE IF EXISTS song;" \
  "DROP TABLE IF EXISTS artist;" \
  "DROP TABLE IF EXISTS album;" \
  "DROP TABLE IF EXISTS genre;" \
  "DROP TABLE IF EXISTS path;"

static const gchar *melo_sort_to_file_db_string[MELO_SORT_COUNT] = {
  [MELO_SORT_FILE] = "file",
  [MELO_SORT_TITLE] = "title",
  [MELO_SORT_ARTIST] = "artist",
  [MELO_SORT_ALBUM] = "album",
  [MELO_SORT_GENRE] = "genre",
  [MELO_SORT_DATE] = "date",
  [MELO_SORT_TRACK] = "track",
  [MELO_SORT_TRACKS] = "tracks",
};

struct _MeloFileDBPrivate {
  GMutex mutex;
  sqlite3 *db;
};

G_DEFINE_TYPE_WITH_PRIVATE (MeloFileDB, melo_file_db, G_TYPE_OBJECT)

static gboolean melo_file_db_open (MeloFileDB *db, const gchar *file);
static void melo_file_db_close (MeloFileDB *db);

static void
melo_file_db_finalize (GObject *gobject)
{
  MeloFileDB *fdb = MELO_FILE_DB (gobject);
  MeloFileDBPrivate *priv = melo_file_db_get_instance_private (fdb);

  /* Close database file */
  melo_file_db_close (fdb);

  /* Clear mutex */
  g_mutex_clear (&priv->mutex);

  /* Chain up to the parent class */
  G_OBJECT_CLASS (melo_file_db_parent_class)->finalize (gobject);
}

static void
melo_file_db_class_init (MeloFileDBClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  /* Add custom finalize() function */
  object_class->finalize = melo_file_db_finalize;
}

static void
melo_file_db_init (MeloFileDB *self)
{
  MeloFileDBPrivate *priv = melo_file_db_get_instance_private (self);

  self->priv = priv;

  /* Init mutex */
  g_mutex_init (&priv->mutex);
}

MeloFileDB *
melo_file_db_new (const gchar *file)
{
  MeloFileDB *fdb;

  /* Create a new object */
  fdb = g_object_new (MELO_TYPE_FILE_DB, NULL);
  if (!fdb)
    return NULL;

  /* Open database file */
  if (!melo_file_db_open (fdb, file)) {
    g_object_unref (fdb);
    return NULL;
  }

  return fdb;
}

static gboolean
melo_file_db_get_int (MeloFileDBPrivate *priv, const gchar *sql, gint *value)
{
  sqlite3_stmt *req;
  gint count = 0;
  int ret;

  /* Prepare SQL request */
  ret = sqlite3_prepare_v2 (priv->db, sql, -1, &req, NULL);
  if (ret != SQLITE_OK)
    return FALSE;

  /* Get value from results */
  while ((ret = sqlite3_step (req)) == SQLITE_ROW) {
    *value = sqlite3_column_int (req, 0);
    count++;
  }

  /* Finalize request */
  sqlite3_finalize (req);

  return ret != SQLITE_DONE || !count ? FALSE : TRUE;
}

static gboolean
melo_file_db_open (MeloFileDB *db, const gchar *file)
{
  MeloFileDBPrivate *priv = db->priv;
  gint version;
  gchar *path;

  /* Create directory if necessary */
  path = g_path_get_dirname (file);
  if (g_mkdir_with_parents (path, 0700)) {
    g_free (path);
    return FALSE;
  }
  g_free (path);

  /* Lock database access */
  g_mutex_lock (&priv->mutex);

  /* Open database file */
  if (!priv->db) {
    /* Open sqlite database */
    if (sqlite3_open (file, &priv->db)) {
      g_mutex_unlock (&priv->mutex);
      return FALSE;
    }

    /* Get database version */
    if (!melo_file_db_get_int (priv, MELO_FILE_DB_GET_VERSION, &version))
      version = 0;

    /* Not initialized or old version */
    if (version < MELO_FILE_DB_VERSION) {
      /* Remove old database */
      sqlite3_exec (priv->db, MELO_FILE_DB_CLEAN, NULL, NULL, NULL);

      /* Initialize database */
      sqlite3_exec (priv->db, MELO_FILE_DB_CREATE, NULL, NULL, NULL);
    }
  }

  /* Unlock database access */
  g_mutex_unlock (&priv->mutex);

  return TRUE;
}

static void
melo_file_db_close (MeloFileDB *db)
{
  MeloFileDBPrivate *priv = db->priv;

  /* Lock database access */
  g_mutex_lock (&priv->mutex);

  /* Close databse */
  if (priv->db) {
    sqlite3_close (priv->db);
    priv->db = NULL;
  }

  /* Unlock database access */
  g_mutex_unlock (&priv->mutex);
}

gboolean
melo_file_db_get_path_id (MeloFileDB *db, const gchar *path, gboolean add,
                          gint *path_id)
{
  MeloFileDBPrivate *priv = db->priv;
  gboolean ret;
  char *sql;

  /* Lock database access */
  g_mutex_lock (&priv->mutex);

  /* Get ID for path */
  sql = sqlite3_mprintf ("SELECT rowid FROM path WHERE path = '%q'", path);
  ret = melo_file_db_get_int (priv, sql, path_id);
  sqlite3_free (sql);

  /* Path not found */
  if (!ret || !*path_id) {
    if (!add) {
      g_mutex_unlock (&priv->mutex);
      return FALSE;
    }

    /* Add new path */
    sql = sqlite3_mprintf ("INSERT INTO path (path) VALUES ('%q')", path);
    sqlite3_exec (priv->db, sql, NULL, NULL, NULL);
    *path_id = sqlite3_last_insert_rowid (priv->db);
    sqlite3_free (sql);
  }

  /* Unlock database access */
  g_mutex_unlock (&priv->mutex);

  return TRUE;
}

gboolean
melo_file_db_add_tags2 (MeloFileDB *db, gint path_id, const gchar *filename,
                        gint timestamp, MeloTags *tags)
{
  const gchar *title, *artist, *album, *genre, *cover;
  MeloFileDBPrivate *priv = db->priv;
  sqlite3_stmt *req;
  guint track = 0, tracks = 0;
  gint row_id = 0, ts = 0;
  gint artist_id;
  gint album_id;
  gint genre_id;
  gint date = 0;
  gboolean ret;
  char *sql, *sql_fts;

  /* Lock database access */
  g_mutex_lock (&priv->mutex);

  /* Find if file is already registered */
  sql = sqlite3_mprintf ("SELECT rowid,timestamp FROM song "
                         "WHERE path_id = %d AND file = '%q'",
                         path_id, filename);
  sqlite3_prepare_v2 (priv->db, sql, -1, &req, NULL);
  sqlite3_free (sql);
  while (sqlite3_step (req) == SQLITE_ROW) {
    row_id = sqlite3_column_int (req, 0);
    ts = sqlite3_column_int (req, 1);
  }
  sqlite3_finalize (req);

  /* File already registered and up to date */
  if (row_id && timestamp == ts) {
    g_mutex_unlock (&priv->mutex);
    return TRUE;
  }

  /* Get strings from tags */
  title = tags && tags->title ? tags->title : NULL;
  artist = tags && tags->artist ? tags->artist : "Unknown";
  album = tags && tags->album ? tags->album : "Unknown";
  genre = tags && tags->genre ? tags->genre : "Unknown";
  cover = tags && tags->cover ? tags->cover : NULL;

  /* Get values from tags */
  if (tags) {
    date = tags->date;
    track = tags->track;
    tracks = tags->tracks;
  }

  /* Find artist ID */
  sql = sqlite3_mprintf ("SELECT rowid FROM artist WHERE artist = '%q'",
                         artist);
  ret = melo_file_db_get_int (priv, sql, &artist_id);
  sqlite3_free (sql);
  if (!ret || !artist_id) {
    /* Add new artist */
    sql = sqlite3_mprintf ("INSERT INTO artist (artist) VALUES ('%q')",
                           artist);
    sqlite3_exec (priv->db, sql, NULL, NULL, NULL);
    artist_id = sqlite3_last_insert_rowid (priv->db);
    sqlite3_free (sql);

    /* Add artist in Full Text Search table */
    sql_fts = sqlite3_mprintf ("INSERT INTO artist_fts (artist) VALUES ('%q')",
                               artist);
    sqlite3_exec (priv->db, sql_fts, NULL, NULL, NULL);
    sqlite3_free (sql_fts);
  }

  /* Find album ID */
  sql = sqlite3_mprintf ("SELECT rowid FROM album WHERE album = '%q'", album);
  ret = melo_file_db_get_int (priv, sql, &album_id);
  sqlite3_free (sql);
  if (!ret || !album_id) {
    /* Add new album */
    sql = sqlite3_mprintf ("INSERT INTO album (album) VALUES ('%q')", album);
    sqlite3_exec (priv->db, sql, NULL, NULL, NULL);
    album_id = sqlite3_last_insert_rowid (priv->db);
    sqlite3_free (sql);

    /* Add album in Full Text Search table */
    sql_fts = sqlite3_mprintf ("INSERT INTO album_fts (album) VALUES ('%q')",
                               album);
    sqlite3_exec (priv->db, sql_fts, NULL, NULL, NULL);
    sqlite3_free (sql_fts);
  }

  /* Find genre ID */
  sql = sqlite3_mprintf ("SELECT rowid FROM genre WHERE genre = '%q'", genre);
  ret = melo_file_db_get_int (priv, sql, &genre_id);
  sqlite3_free (sql);
  if (!ret || !genre_id) {
    /* Add new genre */
    sql = sqlite3_mprintf ("INSERT INTO genre (genre) VALUES ('%q')", genre);
    sqlite3_exec (priv->db, sql, NULL, NULL, NULL);
    genre_id = sqlite3_last_insert_rowid (priv->db);
    sqlite3_free (sql);

    /* Add genre in Full Text Search table */
    sql_fts = sqlite3_mprintf ("INSERT INTO genre_fts (genre) VALUES ('%q')",
                               genre);
    sqlite3_exec (priv->db, sql_fts, NULL, NULL, NULL);
    sqlite3_free (sql_fts);
  }

  /* Add song */
  if (!row_id) {
    sql = sqlite3_mprintf ("INSERT INTO song (title,artist_id,album_id,"
                           "genre_id,date,track,tracks,cover,file,path_id,"
                           "timestamp) "
                           "VALUES (%Q,%d,%d,%d,%d,%d,%d,%Q,'%q',%d,%d)",
                           title, artist_id, album_id, genre_id, date, track,
                           tracks, cover, filename, path_id, timestamp);
    sql_fts = sqlite3_mprintf ("INSERT INTO song_fts (file,title) "
                               "VALUES ('%q',%Q)", filename, title);
  } else {
    sql = sqlite3_mprintf ("UPDATE song SET title = %Q, artist_id = %d, "
                           "album_id = %d, genre_id = %d, date = %d, "
                           "track = %d, tracks = %d, cover = %Q, "
                           "timestamp = '%d' "
                           "WHERE rowid = %d",
                           title, artist_id, album_id, genre_id, date, track,
                           tracks, cover, timestamp, row_id);
    sql_fts = sqlite3_mprintf ("UPDATE song_fts SET title=%Q WHERE rowid = %d",
                               title, row_id);
  }
  sqlite3_exec (priv->db, sql, NULL, NULL, NULL);
  sqlite3_free (sql);

  /* Add song in Full Text Search table */
  sqlite3_exec (priv->db, sql_fts, NULL, NULL, NULL);
  sqlite3_free (sql_fts);

  /* Unlock database access */
  g_mutex_unlock (&priv->mutex);

  return TRUE;
}

gboolean
melo_file_db_add_tags (MeloFileDB *db, const gchar *path, const gchar *filename,
                       gint timestamp, MeloTags *tags)
{
  gint path_id;

  /* Get path ID (and add if not available) */
  if (!melo_file_db_get_path_id (db, path, TRUE, &path_id))
    return FALSE;

  /* Add tags to database */
  return melo_file_db_add_tags2 (db, path_id, filename, timestamp, tags);
}

#define MELO_FILE_DB_COND_SIZE 256
#define MELO_FILE_DB_COLUMN_SIZE 256

static gboolean
melo_file_db_vfind (MeloFileDB *db, MeloFileDBType type, GObject *obj,
                    MeloFileDBGetList cb, gpointer user_data, MeloTags **utags,
                    gint offset, gint count, MeloSort sort, gboolean match,
                    MeloTagsFields tags_fields, MeloFileDBFields field,
                    va_list args)
{
  const gchar *cond_join = match ? " OR " : " AND ";
  const gchar *order = "", *order_col = "", *order_sort = "";
  MeloFileDBPrivate *priv = db->priv;
  sqlite3_stmt *req = NULL;
  gboolean join_artist = FALSE;
  gboolean join_album = FALSE;
  gboolean join_genre = FALSE;
  gboolean join_path = FALSE;
  gboolean join = FALSE;
  GString *conds;
  const gchar *file_cond = NULL;
  const gchar *title_cond = NULL;
  gchar columns[MELO_FILE_DB_COLUMN_SIZE];
  gchar *cols, *conditions;
  gchar *sql;

  /* Prepare string for conditions */
  conds = g_string_new_len (NULL, MELO_FILE_DB_COND_SIZE);
  if (!conds)
    return FALSE;

  /* Generate columns for request */
  cols = g_stpcpy (columns, "m.rowid,");
  if (type == MELO_FILE_DB_TYPE_FILE) {
    cols = g_stpcpy (cols, "path,");
    join_path = TRUE;
  }
  if (type <= MELO_FILE_DB_TYPE_SONG)
    cols = g_stpcpy (cols, "file,");
  if (tags_fields & MELO_TAGS_FIELDS_TITLE)
    cols = g_stpcpy (cols, "title,");
  if (tags_fields & MELO_TAGS_FIELDS_ARTIST) {
    cols = g_stpcpy (cols, "artist,");
    join_artist = TRUE;
  }
  if (tags_fields & MELO_TAGS_FIELDS_ALBUM) {
    cols = g_stpcpy (cols, "album,");
    join_album = TRUE;
  }
  if (tags_fields & MELO_TAGS_FIELDS_GENRE) {
    cols = g_stpcpy (cols, "genre,");
    join_genre = TRUE;
  }
  if (tags_fields & MELO_TAGS_FIELDS_DATE)
    cols = g_stpcpy (cols, "date,");
  if (tags_fields & MELO_TAGS_FIELDS_TRACK)
    cols = g_stpcpy (cols, "track,");
  if (tags_fields & MELO_TAGS_FIELDS_TRACKS)
    cols = g_stpcpy (cols, "tracks,");
  if (tags_fields & MELO_TAGS_FIELDS_COVER)
    cols = g_stpcpy (cols, "m.cover,");
  cols[-1] = '\0';

  /* Generate SQL request */
  while (field != MELO_FILE_DB_FIELDS_END) {
    gchar temp[MELO_FILE_DB_COND_SIZE];
    gboolean skip = FALSE;

    switch (field) {
      case MELO_FILE_DB_FIELDS_PATH:
        sqlite3_snprintf (sizeof (temp), temp, "path = '%q'",
                          va_arg (args, const gchar *));
        join_path = TRUE;
        break;
      case MELO_FILE_DB_FIELDS_PATH_ID:
        sqlite3_snprintf (sizeof (temp), temp, "path_id = '%d'",
                          va_arg (args, gint));
        break;
      case MELO_FILE_DB_FIELDS_FILE:
        if (match) {
          file_cond = va_arg (args, const gchar *);
          skip = TRUE;
        } else {
          sqlite3_snprintf (sizeof (temp), temp, "file = '%q'",
                            va_arg (args, const gchar *));
        }
        break;
      case MELO_FILE_DB_FIELDS_FILE_ID:
        sqlite3_snprintf (sizeof (temp), temp, "m.rowid = '%d'",
                          va_arg (args, gint));
        break;
      case MELO_FILE_DB_FIELDS_TITLE:
        if (match) {
          title_cond = va_arg (args, const gchar *);
          skip = TRUE;
        } else {
          sqlite3_snprintf (sizeof (temp), temp, "title = '%q'",
                            va_arg (args, const gchar *));
        }
        break;
      case MELO_FILE_DB_FIELDS_ARTIST:
        if (match) {
          sqlite3_snprintf (sizeof (temp), temp, "m.artist_id IN ("
                        "SELECT docid FROM artist_fts WHERE artist MATCH '%q')",
                        va_arg (args, const gchar *));
        } else {
          sqlite3_snprintf (sizeof (temp), temp, "artist = '%q'",
                            va_arg (args, const gchar *));
          join_artist = TRUE;
        }
        break;
      case MELO_FILE_DB_FIELDS_ARTIST_ID:
        sqlite3_snprintf (sizeof (temp), temp, "artist_id = '%d'",
                          va_arg (args, gint));
        join = type != MELO_FILE_DB_TYPE_ARTIST;
        break;
      case MELO_FILE_DB_FIELDS_ALBUM:
        if (match) {
          sqlite3_snprintf (sizeof (temp), temp, "m.album_id IN ("
                          "SELECT docid FROM album_fts WHERE album MATCH '%q')",
                          va_arg (args, const gchar *));
        } else {
          sqlite3_snprintf (sizeof (temp), temp, "album = '%q'",
                            va_arg (args, const gchar *));
          join_album = TRUE;
        }
        break;
      case MELO_FILE_DB_FIELDS_ALBUM_ID:
        sqlite3_snprintf (sizeof (temp), temp, "album_id = '%d'",
                          va_arg (args, gint));
        join = type != MELO_FILE_DB_TYPE_ALBUM;
        break;
      case MELO_FILE_DB_FIELDS_GENRE:
        if (match) {
          sqlite3_snprintf (sizeof (temp), temp, "mg.genre_id IN ("
                          "SELECT docid FROM genre_fts WHERE genre MATCH '%q')",
                          va_arg (args, const gchar *));
        } else {
          sqlite3_snprintf (sizeof (temp), temp, "genre = '%q'",
                          va_arg (args, const gchar *));
          join_genre = TRUE;
        }
        break;
      case MELO_FILE_DB_FIELDS_GENRE_ID:
        sqlite3_snprintf (sizeof (temp), temp, "genre_id = '%d'",
                          va_arg (args, gint));
        join = type != MELO_FILE_DB_TYPE_GENRE;
        break;
      case MELO_FILE_DB_FIELDS_DATE:
        sqlite3_snprintf (sizeof (temp), temp, "date = '%d'",
                          va_arg (args, gint));
        break;
      case MELO_FILE_DB_FIELDS_TRACK:
        sqlite3_snprintf (sizeof (temp), temp, "track = '%d'",
                          va_arg (args, gint));
        break;
      case MELO_FILE_DB_FIELDS_TRACKS:
        sqlite3_snprintf (sizeof (temp), temp, "tracks = '%d'",
                          va_arg (args, gint));
        break;
      default:
          g_string_free (conds, TRUE);
        goto error;
    }

    /* Append condition */
    if (!skip)
      g_string_append (conds, temp);

    /* Get next field */
    field = va_arg (args, MeloFileDBFields);
    if (field == MELO_FILE_DB_FIELDS_END)
      break;

    /* Append condition join */
    if (!skip)
      g_string_append (conds, cond_join);
  }

  /* Generate condition for file / title in FTS table */
  if (file_cond || title_cond) {
    gchar temp[MELO_FILE_DB_COND_SIZE];

    /* Append a mix condition for song FTS table */
    sqlite3_snprintf (sizeof (temp), temp,
                 "m.rowid IN (SELECT docid FROM song_fts WHERE %s%q%s%s%q')",
                 file_cond ? "file MATCH '" : "", file_cond ? file_cond : "",
                 file_cond && title_cond ? "' OR " : "",
                 title_cond ? "title MATCH '" : "",
                 title_cond ? title_cond : "");
    g_string_append (conds, temp);
  }

  /* Finalize condition */
  if (!conds->len)
    g_string_append (conds, "1");
  conditions = g_string_free (conds, FALSE);

  /* Generate order directive */
  if (sort != MELO_SORT_NONE && melo_sort_is_valid (sort) &&
      melo_sort_to_file_db_string[melo_sort_set_asc (sort)]) {
    /* Setup order clause */
    order = "ORDER BY ";
    order_col = melo_sort_to_file_db_string[melo_sort_set_asc (sort)];
    if (melo_sort_is_desc (sort))
      order_sort = " COLLATE NOCASE DESC";
    else
      order_sort = " COLLATE NOCASE ASC";
  }

  /* Generate SQL request */
  switch (type) {
    case MELO_FILE_DB_TYPE_SONG:
    case MELO_FILE_DB_TYPE_FILE:
      sql = sqlite3_mprintf ("SELECT %s FROM song m %s %s %s %s "
            "WHERE %s %s%s%s LIMIT %d,%d", columns,
            join_artist ? "LEFT JOIN artist ON m.artist_id = artist.rowid" : "",
            join_album ? "LEFT JOIN album ON m.album_id = album.rowid" : "",
            join_genre ? "LEFT JOIN genre ON m.genre_id = genre.rowid" : "",
            join_path ? "LEFT JOIN path ON m.path_id = path.rowid" : "",
            conditions, order, order_col, order_sort, offset, count);
      break;
    case MELO_FILE_DB_TYPE_ARTIST:
      sql = sqlite3_mprintf ("SELECT DISTINCT %s FROM artist m %s "
                       "WHERE %s %s%s%s LIMIT %d,%d", columns,
                       join ? "LEFT JOIN song ON song.artist_id = m.rowid" : "",
                       conditions, order, order_col, order_sort, offset, count);
      break;
    case MELO_FILE_DB_TYPE_ALBUM:
      sql = sqlite3_mprintf ("SELECT DISTINCT %s FROM album m %s "
                        "WHERE %s %s%s%s LIMIT %d,%d", columns,
                        join ? "LEFT JOIN song ON song.album_id = m.rowid" : "",
                        conditions, order, order_col, order_sort, offset,
                        count);
      break;
    case MELO_FILE_DB_TYPE_GENRE:
      sql = sqlite3_mprintf ("SELECT DISTINCT %s FROM genre m %s "
                       "WHERE %s %s%s%s LIMIT %d,%d", columns,
                        join ? "LEFT JOIN song ON song.genre_id = m.rowid" : "",
                        conditions, order, order_col, order_sort, offset,
                        count);
      break;
    default:
      sql = NULL;
  }
  g_free (conditions);

  /* Do SQL request */
  sqlite3_prepare_v2 (priv->db, sql, -1, &req, NULL);
  sqlite3_free (sql);

  while (sqlite3_step (req) == SQLITE_ROW) {
    const gchar *path = NULL, *file = NULL;
    MeloTags *tags;
    gint id, i = 0;

    /* Do not generate tags */
    if (!cb && (!utags || *utags))
      continue;

    /* Create a new MeloTags */
    tags = melo_tags_new ();
    if (!tags)
      goto error;;

    /* Fill MeloTags */
    id = sqlite3_column_int (req, i++);
    if (type == MELO_FILE_DB_TYPE_FILE)
      path = (const gchar *) sqlite3_column_text (req, i++);
    if (type <= MELO_FILE_DB_TYPE_SONG)
      file = (const gchar *) sqlite3_column_text (req, i++);
    if (tags_fields & MELO_TAGS_FIELDS_TITLE)
      tags->title = g_strdup ((const gchar *) sqlite3_column_text (req, i++));
    if (tags_fields & MELO_TAGS_FIELDS_ARTIST)
      tags->artist = g_strdup ((const gchar *) sqlite3_column_text (req, i++));
    if (tags_fields & MELO_TAGS_FIELDS_ALBUM)
      tags->album = g_strdup ((const gchar *) sqlite3_column_text (req, i++));
    if (tags_fields & MELO_TAGS_FIELDS_GENRE)
      tags->genre = g_strdup ((const gchar *) sqlite3_column_text (req, i++));
    if (tags_fields & MELO_TAGS_FIELDS_DATE)
      tags->date = sqlite3_column_int (req, i++);
    if (tags_fields & MELO_TAGS_FIELDS_TRACK)
      tags->track = sqlite3_column_int (req, i++);
    if (tags_fields & MELO_TAGS_FIELDS_TRACKS)
      tags->tracks = sqlite3_column_int (req, i++);
    if (tags_fields & MELO_TAGS_FIELDS_COVER)
      tags->cover = g_strdup ((const gchar *) sqlite3_column_text (req, i++));

    /* Set utags */
    if (utags && !*utags)
      *utags = tags;

    /* Call callback */
    if (cb && !cb (path, file, id, type, tags, user_data))
      goto error;
  }

  /* Finalize SQL request */
  sqlite3_finalize (req);

  return TRUE;

error:
  if (req)
    sqlite3_finalize (req);
  return FALSE;
}

static MeloTagsFields
melo_file_db_type_get_tags_fields_filter (MeloFileDBType type)
{
  MeloTagsFields filter = MELO_TAGS_FIELDS_COVER;

  if (type <= MELO_FILE_DB_TYPE_SONG)
    return MELO_TAGS_FIELDS_FULL;

  switch (type) {
    case MELO_FILE_DB_TYPE_ARTIST:
      filter |= MELO_TAGS_FIELDS_ARTIST;
      break;
    case MELO_FILE_DB_TYPE_ALBUM:
      filter |= MELO_TAGS_FIELDS_ALBUM;
      break;
    case MELO_FILE_DB_TYPE_GENRE:
      filter |= MELO_TAGS_FIELDS_GENRE;
      break;
    default:
      filter = 0;
  }

  return filter;
}

MeloTags *
melo_file_db_get_tags (MeloFileDB *db, GObject *obj, MeloFileDBType type,
                       MeloTagsFields tags_fields, MeloFileDBFields field_0,
                       ...)
{
  MeloTags *tags = NULL;
  va_list args;

  /* Apply filter on tags */
  tags_fields &= melo_file_db_type_get_tags_fields_filter (type);

  /* Get tags */
  va_start (args, field_0);
  melo_file_db_vfind (db, type, obj, NULL, NULL, &tags, 0, 1, MELO_SORT_NONE,
                      FALSE, tags_fields, field_0,
                      args);
  va_end (args);

  return tags;
}

gboolean
melo_file_db_get_list (MeloFileDB *db, GObject *obj,MeloFileDBGetList cb,
                       gpointer user_data, gint offset, gint count,
                       MeloSort sort, gboolean find, MeloFileDBType type,
                       MeloTagsFields tags_fields, MeloFileDBFields field_0,
                       ...)
{
  gboolean ret;
  va_list args;

  /* Apply filter on tags */
  tags_fields &= melo_file_db_type_get_tags_fields_filter (type);

  /* Get list */
  va_start (args, field_0);
  ret = melo_file_db_vfind (db, type, obj, cb, user_data, NULL, offset, count,
                            sort, find, tags_fields, field_0, args);
  va_end (args);

  return ret;
}
