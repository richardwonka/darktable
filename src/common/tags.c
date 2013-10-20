/*
    This file is part of darktable,
    copyright (c) 2010--2011 henrik andersson.
    copyright (c) 2012 James C. McPherson

    darktable is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    darktable is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "common/darktable.h"
#include "common/tags.h"
#include "common/debug.h"
#include "control/conf.h"
#include "control/control.h"

gboolean dt_tag_new(const char *name,guint *tagid)
{
  int rt;
  guint id = 0;
  sqlite3_stmt *stmt;

  if (!name || name[0] == '\0')
    return FALSE; // no tagid name.

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id FROM tags WHERE name = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, strlen(name), SQLITE_TRANSIENT);
  rt = sqlite3_step(stmt);
  if(rt == SQLITE_ROW)
  {
    // tagid already exists.
    if( tagid != NULL)
      *tagid=sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return  TRUE;
  }
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO tags (id, name) VALUES (null, ?1)", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, strlen(name), SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id FROM tags WHERE name = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, strlen(name), SQLITE_TRANSIENT);
  if (sqlite3_step(stmt) == SQLITE_ROW)
    id = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "INSERT INTO tagxtag SELECT id, ?1, 0 FROM tags", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "UPDATE tagxtag SET count = 1000000 WHERE id1 = ?1 AND id2 = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, id);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if( tagid != NULL)
    *tagid=id;

  return TRUE;
}

gboolean dt_tag_new_from_gui(const char *name,guint *tagid)
{
  gboolean ret = dt_tag_new(name,tagid);
  /* if everything went fine, raise signal of tags change to refresh keywords module in GUI */
  if (ret)
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_TAG_CHANGED);
  return ret;
}

guint dt_tag_remove(const guint tagid, gboolean final)
{
  int rv, count=-1;
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT count() FROM tagged_images WHERE tagid=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
  rv = sqlite3_step(stmt);
  if( rv == SQLITE_ROW)
    count = sqlite3_column_int(stmt,0);
  sqlite3_finalize(stmt);

  if (final == TRUE )
  {
    // let's actually remove the tag
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM tags WHERE id=?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM tagxtag WHERE id1=?1 OR ID2=?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM tagged_images WHERE tagid=?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* raise signal of tags change to refresh keywords module */
    dt_control_signal_raise(darktable.signals, DT_SIGNAL_TAG_CHANGED);

  }

  return count;
}

gchar *dt_tag_get_name(const guint tagid)
{
  int rt;
  char *name=NULL;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT name FROM tags WHERE id= ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1,tagid);
  rt = sqlite3_step(stmt);
  if( rt== SQLITE_ROW )
    name=g_strdup((const char *)sqlite3_column_text(stmt, 0));
  sqlite3_finalize(stmt);

  return name;
}

void dt_tag_reorganize(const gchar *source, const gchar *dest)
{

  if (!strcmp(source,dest)) return;

  char query[1024];
  gchar *tag = g_strrstr(source,"|");

  if (!tag)
    tag = g_strconcat("|", source, NULL);

  if (!strcmp(dest," "))
  {
    tag++;
    dest++;
  }

  g_snprintf(query,1024,
             "UPDATE tags SET name=REPLACE(name,'%s','%s%s') WHERE name LIKE '%s%%'",
             source, dest, tag, source);

  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query, NULL, NULL, NULL);

  /* raise signal of tags change to refresh keywords module */
  //dt_control_signal_raise(darktable.signals, DT_SIGNAL_TAG_CHANGED);
}

gboolean dt_tag_exists(const char *name,guint *tagid)
{
  int rt;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT id FROM tags WHERE name = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_TEXT(stmt, 1, name, strlen(name), SQLITE_TRANSIENT);
  rt = sqlite3_step(stmt);

  if(rt == SQLITE_ROW)
  {
    if( tagid != NULL)
      *tagid = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return  TRUE;
  }

  *tagid = -1;
  sqlite3_finalize(stmt);
  return FALSE;
}

//FIXME: shall we increment count in tagxtag if the image was already tagged?
void dt_tag_attach(guint tagid,gint imgid)
{
  sqlite3_stmt *stmt;
  if(imgid > 0)
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT OR REPLACE INTO tagged_images (imgid, tagid) VALUES (?1, ?2)",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, tagid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE tagxtag SET count = count + 1 WHERE "
                                "(id1 = ?1 AND id2 IN (SELECT tagid FROM tagged_images WHERE imgid = ?2)) "
                                "OR "
                                "(id2 = ?1 AND id1 IN (SELECT tagid FROM tagged_images WHERE imgid = ?2))",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  else
  {
    // insert into tagged_images if not there already.
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "INSERT OR REPLACE INTO tagged_images SELECT imgid, ?1 "
                                "FROM selected_images", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE tagxtag SET count = count + 1 WHERE (id1 = ?1 AND id2 IN "
                                "(SELECT tagid FROM selected_images JOIN tagged_images)) OR "
                                "(id2 = ?1 AND id1 IN (SELECT tagid FROM selected_images "
                                "JOIN tagged_images))",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}

void dt_tag_attach_list(GList *tags,gint imgid)
{
  GList *child=NULL;
  if( (child=g_list_first(tags))!=NULL )
    do
    {
      dt_tag_attach(GPOINTER_TO_INT(child->data), imgid);
    }
    while( (child=g_list_next(child)) !=NULL);
}

void dt_tag_attach_string_list(const gchar *tags, gint imgid)
{
  gchar **tokens = g_strsplit(tags, ",", 0);
  if(tokens)
  {
    gchar **entry = tokens;
    while(*entry)
    {
      // remove leading and trailing spaces
      char *e = *entry + strlen(*entry) - 1;
      while(*e == ' ' && e > *entry) *e = '\0';
      e = *entry;
      while(*e == ' ' && *e != '\0') e++;
      if(*e)
      {
        // add the tag to the image
        guint tagid = 0;
        dt_tag_new(e,&tagid);
        dt_tag_attach(tagid, imgid);
      }
      entry++;
    }
  }
  g_strfreev(tokens);
}

void dt_tag_detach(guint tagid,gint imgid)
{
  sqlite3_stmt *stmt;
  if(imgid > 0)
  {
    // remove from specified image by id
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "UPDATE tagxtag SET count = count - 1 WHERE (id1 = ?1 AND id2 IN "
                                "(SELECT tagid FROM tagged_images WHERE imgid = ?2)) OR (id2 = ?1 "
                                "AND id1 IN (SELECT tagid FROM tagged_images WHERE imgid = ?2))",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // remove from tagged_images
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "DELETE FROM tagged_images WHERE tagid = ?1 AND imgid = ?2",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
  else
  {
    // remove from all selected images
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "update tagxtag set count = count - 1 where (id1 = ?1 and id2 in "
                                "(select tagid from selected_images join tagged_images)) or (id2 = ?1 "
                                "and id1 in (select tagid from selected_images join tagged_images))",
                                -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // remove from tagged_images
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "delete from tagged_images where tagid = ?1 and imgid in "
                                "(select imgid from selected_images)", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, tagid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }
}

void dt_tag_detach_by_string(const char *name, gint imgid)
{
  char query[2048]= {0};
  g_snprintf(query, sizeof(query),
             "DELETE FROM tagged_images WHERE tagid IN (SELECT id FROM "
             "tags WHERE name LIKE '%s') AND imgid = %d;", name, imgid);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query,
                        NULL, NULL, NULL);
}


uint32_t dt_tag_get_attached(gint imgid,GList **result)
{
  sqlite3_stmt *stmt;
  if(imgid > 0)
  {
    char query[1024];
    snprintf(query, sizeof(query),
             "SELECT DISTINCT T.id, T.name FROM tagged_images "
             "JOIN tags T on T.id = tagged_images.tagid "
             "WHERE tagged_images.imgid = %d", imgid);
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), query, -1,
                                &stmt, NULL);
  }
  else
  {
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                                "SELECT DISTINCT T.id, T.name "
                                "FROM tagged_images,tags as T "
                                "WHERE tagged_images.imgid in (select imgid from selected_images)"
                                "  AND T.id = tagged_images.tagid", -1, &stmt, NULL);
  }

  // Create result
  uint32_t count=0;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    dt_tag_t *t=g_malloc(sizeof(dt_tag_t));
    t->id = sqlite3_column_int(stmt, 0);
    t->tag = g_strdup((char *)sqlite3_column_text(stmt, 1));
    *result=g_list_append(*result,t);
    count++;
  }
  sqlite3_finalize(stmt);
  return count;
}

gchar* dt_tag_get_list(gint imgid, const gchar *separator)
{
  gchar *result = NULL;
  GList *taglist = NULL;
  GList *tags = NULL;
  dt_tag_t *t;
  gchar *value = NULL;
  gchar **pch;

  int count = dt_tag_get_attached (imgid, &taglist);

  if (count < 1)
    return NULL;

  for (guint i = 0; i < g_list_length(taglist); i++)
  {

    t = g_list_nth_data (taglist, i);
    value = g_strdup(t->tag);
    if (g_strrstr(value, "|") && !g_str_has_prefix(value, "darktable|"))
    {
      size_t j = 0;
      pch = g_strsplit(value, "|", -1);

      if (pch != NULL)
      {
        while (pch[j] != NULL)
        {
          tags = g_list_prepend(tags, g_strdup(pch[j]));
          j++;
        }
        g_strfreev(pch);
      }
    }
    else if (!g_str_has_prefix(value, "darktable|"))
      tags = g_list_prepend(tags, g_strdup(value));
    g_free (t);
  }

  g_list_free (taglist);

  result = dt_util_glist_to_str (separator, tags, g_list_length(tags));

  return result;
}

gchar *dt_tag_get_hierarchical(gint imgid, const gchar *separator)
{
  GList *taglist = NULL;
  GList *tags = NULL;
  gchar *result = NULL;

  int count = dt_tag_get_attached (imgid, &taglist);

  if (count < 1)
    return NULL;

  for (guint i=0; i<g_list_length(taglist); i++)
  {
    dt_tag_t *t;
    gchar *value = NULL;

    t = g_list_nth_data (taglist, i);
    value = g_strdup(t->tag);

    /* return all tags, but omit the internal darktable ones: */
    if (!g_str_has_prefix(value, "darktable|"))
      tags = g_list_prepend(tags, value);

    g_free (t);
  }

  result = dt_util_glist_to_str (separator, tags, g_list_length(tags));

  return result;
}

/*
 * dt_tag_get_suggestions() takes a string (keyword) and searches the
 * tagxtags table for possibly-related tags. The list we construct at
 * the end of the function is made up as follows:
 *
 * * Tags which appear as tagxtag.id2, where (keyword's name = tagxtag.id1)
 *   are listed first, ordered by count of times seen already.
 * * Tags which appear as tagxtag.id1, where (keyword's name = tagxtag.id2)
 *   are listed second, ordered as before.
 *
 * We do not suggest tags which have not yet been matched up in tagxtag,
 * because it is up to the user to add new tags to the list and thereby
 * make the association.
 *
 * Expressing these as separate queries avoids making the sqlite3 engine
 * do a large number of operations and thus makes the user experience
 * snappy.
 *
 * SELECT T.id FROM tags T WHERE T.name LIKE '?1';  --> into temp table
 * SELECT TXT.id2 FROM tagxtag TXT WHERE TXT.id1 IN (temp table)
 *   AND TXT.count > 0 ORDER BY TXT.count DESC;
 * SELECT TXT.id1 FROM tagxtag TXT WHERE TXT.id2 IN (temp table)
 *   AND TXT.count > 0 ORDER BY TXT.count DESC;
 *
 * SELECT DISTINCT(T.name) FROM tags T JOIN memoryquery MQ on MQ.id = T.id;
 *
 */
uint32_t dt_tag_get_suggestions(const gchar *keyword, GList **result)
{
  sqlite3_stmt *stmt;
  char query[1024];
  /*
   * Earlier versions of this function used a large collation of selects
   * and joins, resulting in multi-*second* timings for sqlite3_exec().
   *
   * Breaking the query into several smaller ones allows the sqlite3
   * execution engine to work more effectively, which is very important
   * for interactive response since we call this function several times
   * in quick succession (on every keystroke).
   */

  /* Quick sanity check - is keyword empty? If so .. return 0 */
  if (keyword == 0)
    return 0;

  /* SELECT T.id FROM tags T WHERE T.name LIKE '%%%s%%';  --> into temp table */
  memset(query, 0, sizeof(query));
  snprintf(query, sizeof(query),
           "INSERT INTO memory.tagq (id) SELECT id FROM tags T WHERE "
           "T.name LIKE '%%%s%%' ", keyword);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query,
                        NULL, NULL, NULL);

  /*
   * SELECT TXT.id2 FROM tagxtag TXT WHERE TXT.id1 IN (temp table)
   *   AND TXT.count > 0 ORDER BY TXT.count DESC;
   */
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "INSERT INTO memory.taglist (id, count) "
                        "SELECT DISTINCT(TXT.id2), TXT.count FROM tagxtag TXT "
                        "WHERE TXT.count > 0 "
                        " AND TXT.id1 IN (SELECT id FROM memory.tagq) "
                        "ORDER BY TXT.count DESC",
                        NULL, NULL, NULL);

  /*
   * SELECT TXT.id1 FROM tagxtag TXT WHERE TXT.id2 IN (temp table)
   *   AND TXT.count > 0 ORDER BY TXT.count DESC;
   */
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "INSERT OR REPLACE INTO memory.taglist (id, count) "
                        "SELECT DISTINCT(TXT.id1), TXT.count FROM tagxtag TXT "
                        "WHERE TXT.count > 0 "
                        " AND TXT.id2 IN (SELECT id FROM memory.tagq) "
                        "ORDER BY TXT.count DESC",
                        NULL, NULL, NULL);

  /* Now put all the bits together */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "SELECT T.name, T.id FROM tags T "
                              "JOIN memory.taglist MT ON MT.id = T.id "
                              "WHERE T.id IN (SELECT DISTINCT(MT.id) FROM memory.taglist MT) "
                              "  AND T.name NOT LIKE 'darktable|%%' "
                              "ORDER BY MT.count DESC",
                              -1, &stmt, NULL);

  /* ... and create the result list to send upwards */
  uint32_t count=0;
  dt_tag_t *t;
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    t = g_malloc(sizeof(dt_tag_t));
    t->tag = g_strdup((char *)sqlite3_column_text(stmt, 0));
    t->id = sqlite3_column_int(stmt, 1);
    *result = g_list_append((*result),t);
    count++;
  }

  sqlite3_finalize(stmt);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "DELETE from memory.taglist", NULL, NULL, NULL);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db),
                        "DELETE from memory.tagq", NULL, NULL, NULL);

  return count;
}

void _free_result_item(dt_tag_t *t,gpointer unused)
{
  g_free(t->tag);
  g_free(t);
}

void dt_tag_free_result(GList **result)
{
  if( result && *result )
  {
    g_list_foreach(*result, (GFunc)_free_result_item , NULL);
    g_list_free(*result);
  }
}

uint32_t dt_tag_get_recent_used(GList **result)
{
  return 0;
}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
