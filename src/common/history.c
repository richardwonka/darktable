/*
    This file is part of darktable,
    copyright (c) 2010 henrik andersson,
    copyright (c) 2011-2012 johannes hanika

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
#include "develop/develop.h"
#include "control/control.h"
#include "common/debug.h"
#include "common/exif.h"
#include "common/history.h"
#include "common/imageio.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/tags.h"
#include "common/utility.h"

static void
remove_preset_flag(const int imgid)
{
  const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, imgid);
  dt_image_t *image = dt_image_cache_write_get(darktable.image_cache, cimg);

  // clear flag
  image->flags &= ~DT_IMAGE_AUTO_PRESETS_APPLIED;

  // write through to sql+xmp
  dt_image_cache_write_release(darktable.image_cache, image, DT_IMAGE_CACHE_SAFE);
  dt_image_cache_read_release(darktable.image_cache, cimg);
}

static void
_dt_history_cleanup_multi_instance(int imgid, int minnum)
{
  sqlite3_stmt *stmt;

  /* let's clean-up the history multi-instance. What we want to do is have a unique multi_priority value for each iop.
     Furthermore this value must start to 0 and increment one by one for each multi-instance of the same module. On
     SQLite there is no notion of ROW_NUMBER, so we use rather resource consuming SQL statement, but as an history has
     never a huge number of items that's not a real issue.

     We only do this for the given imgid and only for num>minnum, that is we only handle new history items just copied.
  */

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "update history set multi_priority=(select COUNT(0)-1 from history hst2 where hst2.num<=history.num and hst2.num>=?2 and hst2.operation=history.operation and hst2.imgid=?1) where imgid=?1 and num>=?2", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, minnum);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);
}

void dt_history_delete_on_image(int32_t imgid)
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from history where imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);
  
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from mask where imgid = ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);
  
  remove_preset_flag(imgid);

  /* if current image in develop reload history */
  if (dt_dev_is_current_image (darktable.develop, imgid))
    dt_dev_reload_history_items (darktable.develop);

  /* make sure mipmaps are recomputed */
  dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);

  /* remove darktable|style|* tags */
  dt_tag_detach_by_string("darktable|style%", imgid);
}

void
dt_history_delete_on_selection()
{
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select * from selected_images", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int (stmt, 0);
    dt_history_delete_on_image (imgid);
  }
  sqlite3_finalize(stmt);
}

int
dt_history_load_and_apply_on_selection (gchar *filename)
{
  int res=0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select * from selected_images", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int(stmt, 0);
    const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, (int32_t)imgid);
    dt_image_t *img = dt_image_cache_write_get(darktable.image_cache, cimg);
    if(img)
    {
      if (dt_exif_xmp_read(img, filename, 1))
      {
        res=1;
        break;
      }

      /* if current image in develop reload history */
      if (dt_dev_is_current_image(darktable.develop, imgid))
        dt_dev_reload_history_items (darktable.develop);

      dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
      dt_image_cache_read_release(darktable.image_cache, img);
      dt_mipmap_cache_remove(darktable.mipmap_cache, imgid);
    }
  }
  sqlite3_finalize(stmt);
  return res;
}

int
dt_history_copy_and_paste_on_image (int32_t imgid, int32_t dest_imgid, gboolean merge, GList *ops)
{
  sqlite3_stmt *stmt;
  if(imgid==dest_imgid) return 1;

  if(imgid==-1)
  {
    dt_control_log(_("you need to copy history from an image before you paste it onto another"));
    return 1;
  }

  /* if merge onto history stack, lets find history offest in destination image */
  int32_t offs = 0;
  if (merge)
  {
    /* apply on top of history stack */
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT MAX(num)+1 FROM history WHERE imgid = ?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
    if (sqlite3_step (stmt) == SQLITE_ROW) offs = sqlite3_column_int (stmt, 0);
  }
  else
  {
    /* replace history stack */
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from history where imgid = ?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
    sqlite3_step (stmt);
  }
  sqlite3_finalize (stmt);

  //  prepare SQL request
  char req[2048];
  strcpy (req, "insert into history (imgid, num, module, operation, op_params, enabled, blendop_params, blendop_version, multi_name, multi_priority) select ?1, num+?2, module, operation, op_params, enabled, blendop_params, blendop_version, multi_name, multi_priority from history where imgid = ?3");

  //  Add ops selection if any format: ... and num in (val1, val2)
  if (ops)
  {
    GList *l = ops;
    int first = 1;
    strcat (req, " and num in (");

    while (l)
    {
      unsigned int value = GPOINTER_TO_UINT(l->data);
      char v[30];

      if (!first) strcat (req, ",");
      snprintf (v, 30, "%u", value);
      strcat (req, v);
      first=0;
      l = g_list_next(l);
    }
    strcat (req, ")");
  }

  /* add the history items to stack offest */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), req, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, offs);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 3, imgid);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  if (merge && ops)
    _dt_history_cleanup_multi_instance(dest_imgid, offs);

  //we have to copy masks too
  //what to do with existing masks ?
  if (merge)
  {
    //there's very little chance that we will have same shapes id.
    //but we may want to handle this case anyway
    //and it's not trivial at all !
  }
  else
  {
    //let's remove all existing shapes
    DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "delete from mask where imgid = ?1", -1, &stmt, NULL);
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
    sqlite3_step (stmt);
    sqlite3_finalize (stmt);
  }

  //let's copy now
  strcpy (req, "insert into mask (imgid, formid, form, name, version, points, points_count, source) select ?1, formid, form, name, version, points, points_count, source from mask where imgid = ?2");
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), req, -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, dest_imgid);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 2, imgid);
  sqlite3_step (stmt);
  sqlite3_finalize (stmt);

  /* if current image in develop reload history */
  if (dt_dev_is_current_image(darktable.develop, dest_imgid))
  {
    dt_dev_reload_history_items (darktable.develop);
    dt_dev_modulegroups_set(darktable.develop, dt_dev_modulegroups_get(darktable.develop));
  }

  /* update xmp file */
  dt_image_synch_xmp(dest_imgid);

  dt_mipmap_cache_remove(darktable.mipmap_cache, dest_imgid);

  return 0;
}

GList *
dt_history_get_items(int32_t imgid, gboolean enabled)
{
  GList *result=NULL;
  sqlite3_stmt *stmt;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select num, operation, enabled, multi_name from history where imgid=?1 and num in (select MAX(num) from history hst2 where hst2.imgid=?1 and hst2.operation=history.operation group by multi_priority) order by num desc", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    char name[512]= {0};
    const int is_active = sqlite3_column_int(stmt, 2);

    if (enabled == FALSE || is_active)
    {
      dt_history_item_t *item=g_malloc (sizeof (dt_history_item_t));
      item->num = sqlite3_column_int (stmt, 0);
      char *mname = NULL;
      mname = g_strdup((gchar *)sqlite3_column_text(stmt, 3));
      if (enabled)
      {
        if (strcmp(mname,"0") == 0) g_snprintf(name,512,"%s",dt_iop_get_localized_name((char*)sqlite3_column_text(stmt, 1)));
        else g_snprintf(name,512,"%s %s",dt_iop_get_localized_name((char*)sqlite3_column_text(stmt, 1)),(char*)sqlite3_column_text(stmt, 3));
      }
      else
      {
        if (strcmp(mname,"0") == 0) g_snprintf(name,512,"%s (%s)",dt_iop_get_localized_name((char*)sqlite3_column_text(stmt, 1)), (is_active!=0)?_("on"):_("off"));
        g_snprintf(name,512,"%s %s (%s)",dt_iop_get_localized_name((char*)sqlite3_column_text(stmt, 1)), (char*)sqlite3_column_text(stmt, 3), (is_active!=0)?_("on"):_("off"));
      }
      item->name = g_strdup (name);
      item->op = g_strdup((gchar *)sqlite3_column_text(stmt, 1));
      result = g_list_append (result,item);

      g_free(mname);
    }
  }
  return result;
}

char *
dt_history_get_items_as_string(int32_t imgid)
{
  GList *items = NULL;
  const char *onoff[2] = {_("off"), _("on")};
  unsigned int count = 0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select operation, enabled from history where imgid=?1 order by num desc", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);

  // collect all the entries in the history from the db
  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    char name[512]= {0};
    g_snprintf(name,512,"%s (%s)", dt_iop_get_localized_name((char*)sqlite3_column_text(stmt, 0)), (sqlite3_column_int(stmt, 1)==0)?onoff[0]:onoff[1]);
    items = g_list_append(items, g_strdup(name));
    count++;
  }
  return dt_util_glist_to_str("\n", items, count);
}

int
dt_history_copy_and_paste_on_selection (int32_t imgid, gboolean merge, GList *ops)
{
  if (imgid < 0) return 1;

  int res=0;
  sqlite3_stmt *stmt;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select * from selected_images where imgid != ?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
  if (sqlite3_step(stmt) == SQLITE_ROW)
  {
    do
    {
      /* get imgid of selected image */
      int32_t dest_imgid = sqlite3_column_int (stmt, 0);

      /* paste history stack onto image id */
      dt_history_copy_and_paste_on_image(imgid,dest_imgid,merge,ops);

    }
    while (sqlite3_step (stmt) == SQLITE_ROW);
  }
  else res = 1;

  sqlite3_finalize(stmt);
  return res;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
