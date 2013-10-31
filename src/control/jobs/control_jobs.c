/*
    This file is part of darktable,
    copyright (c) 2010-2011 johannes hanika
    copyright (c) 2010-2012 henrik andersson

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
#include "common/collection.h"
#include "common/image.h"
#include "common/image_cache.h"
#include "common/mipmap_cache.h"
#include "common/imageio.h"
#include "common/imageio_dng.h"
#include "common/exif.h"
#include "common/film.h"
#include "common/history.h"
#include "common/imageio_module.h"
#include "common/debug.h"
#include "common/tags.h"
#include "common/debug.h"
#include "common/gpx.h"
#include "control/conf.h"
#include "control/jobs/control_jobs.h"

#include "gui/gtk.h"

#include <glib.h>
#include <glib/gstdio.h>
#ifndef __WIN32__
  #include <glob.h>
#endif

#if GLIB_CHECK_VERSION (2, 26, 0)
typedef struct dt_control_time_offset_t
{
  long int offset;
} dt_control_time_offset_t;

typedef struct dt_control_gpx_apply_t
{
  gchar *filename;
  gchar *tz;
} dt_control_gpx_apply_t;
#endif

void dt_control_write_sidecar_files()
{
  dt_job_t j;
  dt_control_write_sidecar_files_job_init(&j);
  dt_control_add_job(darktable.control, &j);
}

void dt_control_write_sidecar_files_job_init(dt_job_t *job)
{
  dt_control_job_init(job, "write sidecar files");
  job->execute = &dt_control_write_sidecar_files_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_selected_init(t);
}

int32_t dt_control_write_sidecar_files_job_run(dt_job_t *job)
{
  int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  while(t)
  {
    gboolean from_cache = FALSE;
    imgid = GPOINTER_TO_INT(t->data);
    const dt_image_t *img = dt_image_cache_read_get(darktable.image_cache, (int32_t)imgid);
    char dtfilename[DT_MAX_PATH_LEN+4];
    dt_image_full_path(img->id, dtfilename, DT_MAX_PATH_LEN, &from_cache);
    char *c = dtfilename + strlen(dtfilename);
    sprintf(c, ".xmp");
    dt_exif_xmp_write(imgid, dtfilename);
    dt_image_cache_read_release(darktable.image_cache, img);
    t = g_list_delete_link(t, t);
  }
  return 0;
}


static float
envelope(const float xx)
{
  const float x = CLAMPS(xx, 0.0f, 1.0f);
  // const float alpha = 2.0f;
  const float beta = 0.5f;
  if(x < beta)
  {
    // return 1.0f-fabsf(x/beta-1.0f)^2
    const float tmp = fabsf(x/beta-1.0f);
    return 1.0f-tmp*tmp;
  }
  else
  {
    const float tmp1 = (1.0f-x)/(1.0f-beta);
    const float tmp2 = tmp1*tmp1;
    const float tmp3 = tmp2*tmp1;
    return 3.0f*tmp2 - 2.0f*tmp3;
  }
}

int32_t dt_control_merge_hdr_job_run(dt_job_t *job)
{
  int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  int total = g_list_length(t);
  char message[512]= {0};
  double fraction=0;
  snprintf(message, 512, ngettext ("merging %d image", "merging %d images", total), total );

  const guint *jid = dt_control_backgroundjobs_create(darktable.control, 1, message);

  float *pixels = NULL;
  float *weight = NULL;
  int wd = 0, ht = 0, first_imgid = -1;
  uint32_t filter = 0;
  float whitelevel = 0.0f;
  total ++;
  while(t)
  {
    imgid = GPOINTER_TO_INT(t->data);
    dt_mipmap_buffer_t buf;
    dt_mipmap_cache_read_get(darktable.mipmap_cache, &buf, imgid, DT_MIPMAP_FULL, DT_MIPMAP_BLOCKING);
    // just take a copy. also do it after blocking read, so filters and bpp will make sense.
    const dt_image_t *img = dt_image_cache_read_get(darktable.image_cache, imgid);
    dt_image_t image = *img;
    dt_image_cache_read_release(darktable.image_cache, img);
    if(image.filters == 0 || image.bpp != sizeof(uint16_t))
    {
      dt_control_log(_("exposure bracketing only works on raw images"));
      dt_mipmap_cache_read_release(darktable.mipmap_cache, &buf);
      free(pixels);
      free(weight);
      goto error;
    }
    filter = dt_image_flipped_filter(img);
    if(buf.size != DT_MIPMAP_FULL)
    {
      dt_control_log(_("failed to get raw buffer from image `%s'"), image.filename);
      dt_mipmap_cache_read_release(darktable.mipmap_cache, &buf);
      free(pixels);
      free(weight);
      goto error;
    }

    if(!pixels)
    {
      first_imgid = imgid;
      pixels = (float *)malloc(sizeof(float)*image.width*image.height);
      weight = (float *)malloc(sizeof(float)*image.width*image.height);
      memset(pixels, 0x0, sizeof(float)*image.width*image.height);
      memset(weight, 0x0, sizeof(float)*image.width*image.height);
      wd = image.width;
      ht = image.height;
    }
    else if(image.width != wd || image.height != ht)
    {
      dt_control_log(_("images have to be of same size!"));
      free(pixels);
      free(weight);
      dt_mipmap_cache_read_release(darktable.mipmap_cache, &buf);
      goto error;
    }
    // if no valid exif data can be found, assume peleng fisheye at f/16, 8mm, with half of the light lost in the system => f/22
    const float eap = image.exif_aperture > 0.0f ? image.exif_aperture : 22.0f;
    const float efl = image.exif_focal_length > 0.0f ? image.exif_focal_length : 8.0f;
    const float rad = .5f * efl/eap;
    const float aperture = M_PI * rad * rad;
    const float iso = image.exif_iso > 0.0f ? image.exif_iso : 100.0f;
    const float exp = image.exif_exposure > 0.0f ? image.exif_exposure : 1.0f;
    const float cal = 100.0f/(aperture*exp*iso);
    // about proportional to how many photons we can expect from this shot:
    const float photoncnt = 100.0f*aperture*exp/iso;
    // stupid, but we don't know the real sensor saturation level:
    uint16_t saturation = 0;
    for(int k=0; k<wd*ht; k++)
      saturation = MAX(saturation, ((uint16_t *)buf.buf)[k]);
    // seems to be around 64500--64700 for 5dm2
    // fprintf(stderr, "saturation: %u\n", saturation);
    whitelevel = fmaxf(whitelevel, saturation*cal);
#ifdef _OPENMP
    #pragma omp parallel for schedule(static) default(none) shared(buf, pixels, weight, wd, ht, saturation)
#endif
    for(int k=0; k<wd*ht; k++)
    {
      const uint16_t in = ((uint16_t *)buf.buf)[k];
      // weights based on siggraph 12 poster
      // zijian zhu, zhengguo li, susanto rahardja, pasi fraenti
      // 2d denoising factor for high dynamic range imaging
      float w = envelope(in/(float)saturation) * photoncnt;
      // in case we are black and drop to zero weight, give it something
      // just so numerics don't collapse. blown out whites are handled below.
      if(w < 1e-3f && in < saturation/3) w = 1e-3f;
      pixels[k] += w * in * cal;
      weight[k] += w;
    }

    t = g_list_delete_link(t, t);

    /* update backgroundjob ui plate */
    fraction+=1.0/total;
    dt_control_backgroundjobs_progress(darktable.control, jid, fraction);

    dt_mipmap_cache_read_release(darktable.mipmap_cache, &buf);
  }
  // normalize by white level to make clipping at 1.0 work as expected (to be sure, scale down one more stop, thus the 0.5):
#ifdef _OPENMP
  #pragma omp parallel for schedule(static) default(none) shared(pixels, wd, ht, weight, whitelevel)
#endif
  for(int k=0; k<wd*ht; k++)
  {
    // in case w == 0, all pixels were overexposed (too dark would have been clamped to w >= eps above)
    if(weight[k] < 1e-3f)
      pixels[k] = 1.f; // mark as blown out.
    else // normalize:
      pixels[k] = fmaxf(0.0f, pixels[k]/(whitelevel*weight[k]));
  }

  // output hdr as digital negative with exif data.
  uint8_t exif[65535];
  char pathname[DT_MAX_PATH_LEN];
  gboolean from_cache = TRUE;
  dt_image_full_path(first_imgid, pathname, DT_MAX_PATH_LEN, &from_cache);
  // last param is dng mode
  const int exif_len = dt_exif_read_blob(exif, pathname, first_imgid, 0, wd, ht, 1);
  char *c = pathname + strlen(pathname);
  while(*c != '.' && c > pathname) c--;
  g_strlcpy(c, "-hdr.dng", sizeof(pathname)-(c-pathname));
  dt_imageio_write_dng(pathname, pixels, wd, ht, exif, exif_len, filter, 1.0f);

  dt_control_backgroundjobs_progress(darktable.control, jid, 1.0f);

  while(*c != '/' && c > pathname) c--;
  dt_control_log(_("wrote merged HDR `%s'"), c+1);

  // import new image
  gchar *directory = g_path_get_dirname((const gchar *)pathname);
  dt_film_t film;
  const int filmid = dt_film_new(&film, directory);
  dt_image_import(filmid, pathname, TRUE);
  g_free (directory);

  free(pixels);
  free(weight);
error:
  dt_control_backgroundjobs_destroy(darktable.control, jid);
  dt_control_queue_redraw_center();
  return 0;
}

int32_t dt_control_duplicate_images_job_run(dt_job_t *job)
{
  int imgid = -1;
  int newimgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  int total = g_list_length(t);
  char message[512]= {0};
  double fraction=0;
  snprintf(message, 512, ngettext ("duplicating %d image", "duplicating %d images", total), total );
  const guint *jid = dt_control_backgroundjobs_create(darktable.control, 0, message);
  while(t)
  {
    imgid = GPOINTER_TO_INT(t->data);
    newimgid = dt_image_duplicate(imgid);
    if(newimgid != -1) dt_history_copy_and_paste_on_image(imgid, newimgid, FALSE,NULL);
    t = g_list_delete_link(t, t);
    fraction=1.0/total;
    dt_control_backgroundjobs_progress(darktable.control, jid, fraction);
  }
  dt_control_backgroundjobs_destroy(darktable.control, jid);
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
  dt_control_queue_redraw_center();
  return 0;
}

int32_t dt_control_flip_images_job_run(dt_job_t *job)
{
  int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  const int cw = t1->flag;
  GList *t = t1->index;
  int total = g_list_length(t);
  double fraction=0;
  char message[512]= {0};
  snprintf(message, 512, ngettext ("flipping %d image", "flipping %d images", total), total );
  const guint *jid = dt_control_backgroundjobs_create(darktable.control, 0, message);
  while(t)
  {
    imgid = GPOINTER_TO_INT(t->data);
    dt_image_flip(imgid, cw);
    t = g_list_delete_link(t, t);
    fraction=1.0/total;
    dt_control_backgroundjobs_progress(darktable.control, jid, fraction);
  }
  dt_control_backgroundjobs_destroy(darktable.control, jid);
  dt_control_queue_redraw_center();
  return 0;
}

int32_t dt_control_remove_images_job_run(dt_job_t *job)
{
  int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  int total = g_list_length(t);
  char message[512]= {0};
  double fraction=0;
  snprintf(message, 512, ngettext ("removing %d image", "removing %d images", total), total );
  const guint *jid = dt_control_backgroundjobs_create(darktable.control, 0, message);
  sqlite3_stmt *stmt = NULL;

  // check that we can safely remove the image
  char query[1024];
  gboolean remove_ok = TRUE;
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "SELECT id FROM images WHERE id IN (SELECT imgid FROM selected_images) AND flags&?1=?1", -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, DT_IMAGE_LOCAL_COPY);

  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int(stmt, 0);
    if (!dt_image_safe_remove(imgid))
    {
      remove_ok = FALSE;
      break;
    }
  }
  sqlite3_finalize(stmt);

  if (!remove_ok)
  {
    dt_control_log(_("cannot remove local copy when the original file is not accessible."));
    dt_control_backgroundjobs_destroy(darktable.control, jid);
    return 0;
  }

  // update remove status
  sprintf(query, "update images set flags = (flags | %d) where id in (select imgid from selected_images)",DT_IMAGE_REMOVE);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query, NULL, NULL, NULL);

  dt_collection_update(darktable.collection);

  // We need a list of files to regenerate .xmp files if there are duplicates
  GList *list = NULL;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select distinct folder || '/' || filename from images, film_rolls where images.film_id = film_rolls.id and images.id in (select imgid from selected_images)", -1, &stmt, NULL);
  while(sqlite3_step(stmt) == SQLITE_ROW)
  {
    list = g_list_append(list, g_strdup((const gchar *)sqlite3_column_text(stmt, 0)));
  }
  sqlite3_finalize(stmt);

  while(t)
  {
    imgid = GPOINTER_TO_INT(t->data);
    dt_image_remove(imgid);
    t = g_list_delete_link(t, t);
    fraction=1.0/total;
    dt_control_backgroundjobs_progress(darktable.control, jid, fraction);
  }

  char *imgname;
  while(list)
  {
    imgname = (char *)list->data;
    dt_image_synch_all_xmp(imgname);
    list = g_list_delete_link(list, list);
  }
  dt_control_backgroundjobs_destroy(darktable.control, jid);
  dt_film_remove_empty();
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
  dt_control_queue_redraw_center();
  return 0;
}


int32_t dt_control_delete_images_job_run(dt_job_t *job)
{
  int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  int total = g_list_length(t);
  char message[512]= {0};
  double fraction=0;
  snprintf(message, 512, ngettext ("deleting %d image", "deleting %d images", total), total );
  const guint *jid = dt_control_backgroundjobs_create(darktable.control, 0, message);

  sqlite3_stmt *stmt;

  char query[1024];
  sprintf(query, "update images set flags = (flags | %d) where id in (select imgid from selected_images)",DT_IMAGE_REMOVE);
  DT_DEBUG_SQLITE3_EXEC(dt_database_get(darktable.db), query, NULL, NULL, NULL);

  dt_collection_update(darktable.collection);

  // We need a list of files to regenerate .xmp files if there are duplicates
  GList *list = NULL;

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select distinct folder || '/' || filename from images, film_rolls where images.film_id = film_rolls.id and images.id in (select imgid from selected_images)", -1, &stmt, NULL);

  if(sqlite3_step(stmt) == SQLITE_ROW)
  {
    list = g_list_append(list, g_strdup((const gchar *)sqlite3_column_text(stmt, 0)));
  }
  sqlite3_finalize(stmt);

  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db), "select count(id) from images where filename in (select filename from images where id = ?1) and film_id in (select film_id from images where id = ?1)", -1, &stmt, NULL);
  while(t)
  {
    imgid = GPOINTER_TO_INT(t->data);
    char filename[DT_MAX_PATH_LEN];
    gboolean from_cache = FALSE;
    dt_image_full_path(imgid, filename, DT_MAX_PATH_LEN, &from_cache);

    int duplicates = 0;
    DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, imgid);
    if(sqlite3_step(stmt) == SQLITE_ROW)
      duplicates = sqlite3_column_int(stmt, 0);
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);

    // remove from disk:
    if(duplicates == 1)
    {
      // there are no further duplicates so we can remove the source data file
      (void)g_unlink(filename);
      dt_image_remove(imgid);

      // all sidecar files - including left-overs - can be deleted;
      // left-overs can result when previously duplicates have been REMOVED;
      // no need to keep them as the source data file is gone.
      const int len = DT_MAX_PATH_LEN + 30;
      gchar pattern[len];

      // NULL terminated list of glob patterns; should include "" and can be extended if needed
      static const gchar *glob_patterns[] = { "", "_[0-9][0-9]", "_[0-9][0-9][0-9]", "_[0-9][0-9][0-9][0-9]", NULL };

      const gchar **glob_pattern = glob_patterns;
      GList *files = NULL;
      while(*glob_pattern)
      {
        snprintf(pattern, len, "%s", filename);
        gchar *c1 = pattern + strlen(pattern);
        while(*c1 != '.' && c1 > pattern) c1--;
        snprintf(c1, pattern + len - c1, "%s", *glob_pattern);
        const gchar *c2 = filename + strlen(filename);
        while(*c2 != '.' && c2 > filename) c2--;
        snprintf(c1+strlen(*glob_pattern), pattern + len - c1 - strlen(*glob_pattern), "%s.xmp", c2);

#ifdef __WIN32__
        WIN32_FIND_DATA data;
        HANDLE handle = FindFirstFile(pattern, &data);
        if(handle != INVALID_HANDLE_VALUE)
        {
          do
            files = g_list_append(files, g_strdup(data.cFileName));
          while(FindNextFile(handle, &data));
        }
#else
        glob_t globbuf;
        if(!glob(pattern, 0, NULL, &globbuf))
        {
          for(size_t i=0; i < globbuf.gl_pathc; i++)
            files = g_list_append(files, g_strdup(globbuf.gl_pathv[i]));
          globfree(&globbuf);
        }
#endif

        glob_pattern++;
      }

      GList *file_iter = g_list_first(files);
      while(file_iter != NULL)
      {
        (void)g_unlink(file_iter->data);
        file_iter = g_list_next(file_iter);
      }

      g_list_free_full(files, g_free);
    }
    else
    {
      // don't remove the actual source data if there are further duplicates using it;
      // just delete the xmp file of the duplicate selected.

      dt_image_path_append_version(imgid, filename, DT_MAX_PATH_LEN);
      char *c = filename + strlen(filename);
      sprintf(c, ".xmp");

      dt_image_remove(imgid);
      (void)g_unlink(filename);
    }

    t = g_list_delete_link(t, t);
    fraction=1.0/total;
    dt_control_backgroundjobs_progress(darktable.control, jid, fraction);
  }
  sqlite3_finalize(stmt);

  char *imgname;
  while(list)
  {
    imgname = (char *)list->data;
    dt_image_synch_all_xmp(imgname);
    list = g_list_delete_link(list, list);
  }
  g_list_free(list);
  dt_control_backgroundjobs_destroy(darktable.control, jid);
  dt_film_remove_empty();
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
  dt_control_queue_redraw_center();
  return 0;
}

#if GLIB_CHECK_VERSION (2, 26, 0)
int32_t dt_control_gpx_apply_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  struct dt_gpx_t *gpx = NULL;
  uint32_t cntr = 0;
  const dt_control_gpx_apply_t *d = t1->data;
  const gchar *filename = d->filename;
  const gchar *tz = d->tz;

  /* do we have any selected images */
  if (!t)
    goto bail_out;

  /* try parse the gpx data */
  gpx = dt_gpx_new(filename);
  if (!gpx)
  {
    dt_control_log(_("failed to parse GPX file"));
    goto bail_out;
  }

  GTimeZone *tz_camera = (tz == NULL)?g_time_zone_new_utc():g_time_zone_new(tz);
  if(!tz_camera)
    goto bail_out;
  GTimeZone *tz_utc = g_time_zone_new_utc();

  /* go thru each selected image and lookup location in gpx */
  do
  {
    GTimeVal timestamp;
    GDateTime *exif_time, *utc_time;
    gdouble lon,lat;
    int imgid = GPOINTER_TO_INT(t->data);

    /* get image */
    const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, imgid);
    if (!cimg)
      continue;

    /* convert exif datetime
       TODO: exiv2 dates should be iso8601 and we are probably doing some ugly
       conversion before inserting into database.
     */
    gint year;
    gint month;
    gint day;
    gint hour;
    gint minute;
    gint  seconds;

    if (sscanf(cimg->exif_datetime_taken, "%d:%d:%d %d:%d:%d",
               (int*)&year, (int*)&month, (int*)&day,
               (int*)&hour,(int*)&minute,(int*)&seconds) != 6)
    {
      fprintf(stderr,"broken exif time in db, '%s'\n", cimg->exif_datetime_taken);
      dt_image_cache_read_release(darktable.image_cache, cimg);
      continue;
    }

    /* release the lock */
    dt_image_cache_read_release(darktable.image_cache, cimg);

    exif_time = g_date_time_new(tz_camera, year, month, day, hour, minute, seconds);
    if(!exif_time)
      continue;
    utc_time = g_date_time_to_timezone(exif_time, tz_utc);
    g_date_time_unref(exif_time);
    if(!utc_time)
      continue;
    gboolean res = g_date_time_to_timeval(utc_time, &timestamp);
    g_date_time_unref(utc_time);
    if(!res)
      continue;

    /* only update image location if time is within gpx tack range */
    if(dt_gpx_get_location(gpx, &timestamp, &lon, &lat))
    {
      dt_image_set_location(imgid, lon, lat);
      cntr++;
    }

  }
  while((t = g_list_next(t)) != NULL);

  dt_control_log(_("applied matched GPX location onto %d image(s)"), cntr);

  g_time_zone_unref(tz_camera);
  g_time_zone_unref(tz_utc);
  dt_gpx_destroy(gpx);
  g_free(d->filename);
  g_free(d->tz);
  g_free(t1->data);
  return 0;

bail_out:
  if (gpx)
    dt_gpx_destroy(gpx);

  g_free(d->filename);
  g_free(d->tz);
  g_free(t1->data);
  return 1;
}
#endif

/* enumerator of images from filmroll */
void dt_control_image_enumerator_job_film_init(dt_control_image_enumerator_t *t, int32_t filmid)
{
  sqlite3_stmt *stmt;
  /* get a list of images in filmroll */
  DT_DEBUG_SQLITE3_PREPARE_V2(dt_database_get(darktable.db),
                              "select * from images where film_id = ?1",
                              -1, &stmt, NULL);
  DT_DEBUG_SQLITE3_BIND_INT(stmt, 1, filmid);

  while (sqlite3_step(stmt) == SQLITE_ROW)
  {
    int imgid = sqlite3_column_int(stmt, 0);
    t->index = g_list_append(t->index, GINT_TO_POINTER(imgid));
  }
  sqlite3_finalize(stmt);
}

/* enumerator of selected images */
void dt_control_image_enumerator_job_selected_init(dt_control_image_enumerator_t *t)
{
  GList *list = NULL;
  int imgid = dt_view_get_image_to_act_on();

  if(imgid < 0) {
    /* get sorted list of selected images */
    t->index = dt_collection_get_selected(darktable.collection, -1);
  }
  else {
    /* Create a list with only one image */
    list = g_list_append (list, GINT_TO_POINTER(imgid));
    t->index = list;
  }
}

void dt_control_merge_hdr_job_init(dt_job_t *job)
{
  dt_control_job_init(job, "merge hdr image");
  job->execute = &dt_control_merge_hdr_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_selected_init(t);
}

void dt_control_duplicate_images_job_init(dt_job_t *job)
{
  dt_control_job_init(job, "duplicate images");
  job->execute = &dt_control_duplicate_images_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_selected_init(t);
}

void dt_control_flip_images_job_init(dt_job_t *job, const int32_t cw)
{
  dt_control_job_init(job, "flip images");
  job->execute = &dt_control_flip_images_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_selected_init(t);
  t->flag = cw;
}

void dt_control_remove_images_job_init(dt_job_t *job)
{
  dt_control_job_init(job, "remove images");
  job->execute = &dt_control_remove_images_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_selected_init(t);
}

void dt_control_delete_images_job_init(dt_job_t *job)
{
  dt_control_job_init(job, "delete images");
  job->execute = &dt_control_delete_images_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_selected_init(t);
}

#if GLIB_CHECK_VERSION (2, 26, 0)
void dt_control_gpx_apply_job_init(dt_job_t *job, const gchar *filename, int32_t filmid, const gchar *tz)
{
  dt_control_job_init(job, "gpx apply");
  job->execute = &dt_control_gpx_apply_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  if (filmid != -1)
    dt_control_image_enumerator_job_film_init(t, filmid);
  else
    dt_control_image_enumerator_job_selected_init(t);

  dt_control_gpx_apply_t *data = (dt_control_gpx_apply_t*)malloc(sizeof(dt_control_gpx_apply_t));
  data->filename = g_strdup(filename);
  data->tz = g_strdup(tz);
  t->data = data;
}
#endif

void dt_control_merge_hdr()
{
  dt_job_t j;
  dt_control_merge_hdr_job_init(&j);
  dt_control_add_job(darktable.control, &j);
}

#if GLIB_CHECK_VERSION (2, 26, 0)
void dt_control_gpx_apply(const gchar *filename, int32_t filmid, const gchar *tz)
{
  dt_job_t j;
  dt_control_gpx_apply_job_init(&j, filename, filmid, tz);
  dt_control_add_job(darktable.control, &j);
}
#endif

void dt_control_duplicate_images()
{
  dt_job_t j;
  dt_control_duplicate_images_job_init(&j);
  dt_control_add_job(darktable.control, &j);
}

void dt_control_flip_images(const int32_t cw)
{
  dt_job_t j;
  dt_control_flip_images_job_init(&j, cw);
  dt_control_add_job(darktable.control, &j);
}

void dt_control_remove_images()
{
  if(dt_conf_get_bool("ask_before_remove"))
  {
    GtkWidget *dialog;
    GtkWidget *win = dt_ui_main_window(darktable.gui->ui);

    int number = dt_collection_get_selected_count(darktable.collection);

    // Do not show the dialog if no image is selected:
    if(number == 0) return;

    dialog = gtk_message_dialog_new(GTK_WINDOW(win),
                                    GTK_DIALOG_DESTROY_WITH_PARENT,
                                    GTK_MESSAGE_QUESTION,
                                    GTK_BUTTONS_YES_NO,
                                    ngettext("do you really want to remove %d selected image from the collection?",
                                        "do you really want to remove %d selected images from the collection?", number), number);

    gtk_window_set_title(GTK_WINDOW(dialog), _("remove images?"));
    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if(res != GTK_RESPONSE_YES) return;
  }
  dt_job_t j;
  dt_control_remove_images_job_init(&j);
  dt_control_add_job(darktable.control, &j);
}

void dt_control_delete_images()
{
  if(dt_conf_get_bool("ask_before_delete"))
  {
    GtkWidget *dialog;
    GtkWidget *win = dt_ui_main_window(darktable.gui->ui);

    int number = dt_collection_get_selected_count(darktable.collection);

    // Do not show the dialog if no image is selected:
    if(number == 0) return;

    dialog = gtk_message_dialog_new(GTK_WINDOW(win),
                                    GTK_DIALOG_DESTROY_WITH_PARENT,
                                    GTK_MESSAGE_QUESTION,
                                    GTK_BUTTONS_YES_NO,
                                    ngettext("do you really want to physically delete %d selected image from disk?",
                                        "do you really want to physically delete %d selected images from disk?", number), number);

    gtk_window_set_title(GTK_WINDOW(dialog), _("delete images?"));
    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    if(res != GTK_RESPONSE_YES) return;
  }
  dt_job_t j;
  dt_control_delete_images_job_init(&j);
  dt_control_add_job(darktable.control, &j);
}

static int32_t _generic_dt_control_fileop_images_job_run(dt_job_t *job,
    int32_t (*fileop_callback)(const int32_t, const int32_t),
    const char *desc, const char *desc_pl)
{
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  int total = g_list_length(t);
  char message[512]= {0};
  double fraction = 0;
  gchar *newdir = (gchar *)job->user_data;

  /* create a cancellable bgjob ui template */
  g_snprintf(message, 512, ngettext(desc, desc_pl, total), total);
  const guint *jid = dt_control_backgroundjobs_create(darktable.control, 0, message);
  dt_control_backgroundjobs_set_cancellable(darktable.control, jid, job);

  // create new film roll for the destination directory
  dt_film_t new_film;
  const int32_t film_id = dt_film_new(&new_film, newdir);
  g_free(newdir);

  if (film_id <= 0)
  {
    dt_control_log(_("failed to create film roll for destination directory, aborting move.."));
    dt_control_backgroundjobs_destroy(darktable.control, jid);
    return -1;
  }

  while(t && dt_control_job_get_state(job) != DT_JOB_STATE_CANCELLED)
  {
    fileop_callback(GPOINTER_TO_INT(t->data), film_id);
    t = g_list_delete_link(t, t);
    fraction+=1.0/total;
    dt_control_backgroundjobs_progress(darktable.control, jid, fraction);
  }

  char collect[1024];
  snprintf(collect, 1024, "1:0:0:%s$", new_film.dirname);
  dt_collection_deserialize(collect);
  dt_control_backgroundjobs_destroy(darktable.control, jid);
  dt_film_remove_empty();
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
  dt_control_queue_redraw_center();
  return 0;
}

void dt_control_move_images()
{
  // Open file chooser dialog
  gchar *dir = NULL;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  int number = dt_collection_get_selected_count(darktable.collection);

  // Do not show the dialog if no image is selected:
  if(number == 0) return;

  GtkWidget *filechooser = gtk_file_chooser_dialog_new (_("select directory"),
                           GTK_WINDOW (win),
                           GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                           GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                           GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
                           (char *)NULL);

  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);
  if (gtk_dialog_run (GTK_DIALOG (filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    dir = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (filechooser));
  }
  gtk_widget_destroy (filechooser);

  if(!dir || !g_file_test(dir, G_FILE_TEST_IS_DIR))
    goto abort;

  if(dt_conf_get_bool("ask_before_move"))
  {
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(win),
                        GTK_DIALOG_DESTROY_WITH_PARENT,
                        GTK_MESSAGE_QUESTION,
                        GTK_BUTTONS_YES_NO,
                        ngettext("do you really want to physically move the %d selected image to %s?\n"
                                 "(all unselected duplicates will be moved along)",
                                 "do you really want to physically move %d selected images to %s?\n"
                                 "(all unselected duplicates will be moved along)", number), number, dir);
    gtk_window_set_title(GTK_WINDOW(dialog), ngettext("move image?", "move images?", number));

    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if(res != GTK_RESPONSE_YES)
      goto abort;
  }

  dt_job_t j;
  dt_control_move_images_job_init(&j);
  j.user_data = dir;
  dt_control_add_job(darktable.control, &j);
  return;

abort:
  g_free(dir);
  return;
}

void dt_control_copy_images()
{
  // Open file chooser dialog
  gchar *dir = NULL;
  GtkWidget *win = dt_ui_main_window(darktable.gui->ui);
  int number = dt_collection_get_selected_count(darktable.collection);

  // Do not show the dialog if no image is selected:
  if(number == 0) return;

  GtkWidget *filechooser = gtk_file_chooser_dialog_new (_("select directory"),
                           GTK_WINDOW (win),
                           GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                           GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
                           GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
                           (char *)NULL);

  gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(filechooser), FALSE);
  if (gtk_dialog_run (GTK_DIALOG (filechooser)) == GTK_RESPONSE_ACCEPT)
  {
    dir = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (filechooser));
  }
  gtk_widget_destroy (filechooser);

  if(!dir || !g_file_test(dir, G_FILE_TEST_IS_DIR))
    goto abort;

  if(dt_conf_get_bool("ask_before_copy"))
  {
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(win),
                        GTK_DIALOG_DESTROY_WITH_PARENT,
                        GTK_MESSAGE_QUESTION,
                        GTK_BUTTONS_YES_NO,
                        ngettext("do you really want to physically copy the %d selected image to %s?",
                                 "do you really want to physically copy %d selected images to %s?", number),
                        number, dir);
    gtk_window_set_title(GTK_WINDOW(dialog), ngettext("copy image?", "copy images?", number));

    gint res = gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);

    if(res != GTK_RESPONSE_YES)
      goto abort;
  }

  dt_job_t j;
  dt_control_copy_images_job_init(&j);
  j.user_data = dir;
  dt_control_add_job(darktable.control, &j);
  return;

abort:
  g_free(dir);
  return;
}

void dt_control_set_local_copy_images()
{
  dt_job_t j;
  dt_control_local_copy_images_job_init(&j);
  j.user_data=(void *)1;
  dt_control_add_job(darktable.control, &j);
  return;
}

void dt_control_reset_local_copy_images()
{
  dt_job_t j;
  dt_control_local_copy_images_job_init(&j);
  j.user_data=(void *)0;
  dt_control_add_job(darktable.control, &j);
  return;
}

void dt_control_move_images_job_init(dt_job_t *job)
{
  dt_control_job_init(job, "move images");
  job->execute = &dt_control_move_images_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_selected_init(t);
}

void dt_control_copy_images_job_init(dt_job_t *job)
{
  dt_control_job_init(job, "copy images");
  job->execute = &dt_control_copy_images_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_selected_init(t);
}

void dt_control_local_copy_images_job_init(dt_job_t *job)
{
  dt_control_job_init(job, "local copy images");
  job->execute = &dt_control_local_copy_images_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  dt_control_image_enumerator_job_selected_init(t);
}

int32_t dt_control_move_images_job_run(dt_job_t *job)
{
  return _generic_dt_control_fileop_images_job_run(job, &dt_image_move,
         _("moving %d image"), _("moving %d images"));
}

int32_t dt_control_copy_images_job_run(dt_job_t *job)
{
  return _generic_dt_control_fileop_images_job_run(job, &dt_image_copy,
         _("copying %d image"), _("copying %d images"));
}

int32_t dt_control_local_copy_images_job_run(dt_job_t *job)
{
  int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  GList *t = t1->index;
  guint tagid = 0;
  const int total = g_list_length(t);
  double fraction=0;
  gboolean is_copy = GPOINTER_TO_INT(job->user_data) == 1;
  char message[512]= {0};

  if (is_copy)
    snprintf(message, 512, ngettext ("creating local copy of %d image", "creating local copies of %d images", total), total);
  else
    snprintf(message, 512, ngettext ("removing local copy of %d image", "removing local copies of %d images", total), total);

  dt_control_log(message);

  dt_tag_new("darktable|local-copy",&tagid);

  /* create a cancellable bgjob ui template */
  const guint *jid = dt_control_backgroundjobs_create(darktable.control, 0, message);
  dt_control_backgroundjobs_set_cancellable(darktable.control, jid, job);
  const dt_control_t *control = darktable.control;

  while(t && dt_control_job_get_state(job) != DT_JOB_STATE_CANCELLED)
  {
    imgid = GPOINTER_TO_INT(t->data);
    if (GPOINTER_TO_INT(job->user_data) == 1)
    {
      dt_image_local_copy_set(imgid);
      dt_tag_attach(tagid, imgid);
    }
    else
    {
      dt_image_local_copy_reset(imgid);
      dt_tag_detach(tagid, imgid);
    }
    t = g_list_delete_link(t, t);

    fraction += 1.0/total;
    dt_control_backgroundjobs_progress(control, jid, fraction);
  }

  dt_control_backgroundjobs_destroy(control, jid);
  dt_control_signal_raise(darktable.signals, DT_SIGNAL_FILMROLLS_CHANGED);
  return 0;
}

static int32_t dt_control_export_job_run(dt_job_t *job)
{
  int imgid = -1;
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  dt_control_export_t *settings = (dt_control_export_t*)t1->data;
  GList *t = t1->index;
  const int total = g_list_length(t);
  dt_imageio_module_format_t  *mformat  = dt_imageio_get_format_by_index(settings->format_index);
  g_assert(mformat);
  dt_imageio_module_storage_t *mstorage = dt_imageio_get_storage_by_index(settings->storage_index);
  g_assert(mstorage);

  // Get max dimensions...
  uint32_t w,h,fw,fh,sw,sh;
  fw=fh=sw=sh=0;
  mstorage->dimension(mstorage, &sw,&sh);
  mformat->dimension(mformat, &fw,&fh);

  if( sw==0 || fw==0) w=sw>fw?sw:fw;
  else w=sw<fw?sw:fw;

  if( sh==0 || fh==0) h=sh>fh?sh:fh;
  else h=sh<fh?sh:fh;

  // get shared storage param struct (global sequence counter, one picasa connection etc)
  dt_imageio_module_data_t *sdata = mstorage->get_params(mstorage);
  if(sdata == NULL)
  {
    dt_control_log(_("failed to get parameters from storage module `%s', aborting export.."), mstorage->name(mstorage));
    g_free(t1->data);
    return 1;
  }
  dt_control_log(ngettext ("exporting %d image..", "exporting %d images..", total), total);
  char message[512]= {0};
  snprintf(message, 512, ngettext ("exporting %d image to %s", "exporting %d images to %s", total), total, mstorage->name(mstorage) );

  /* create a cancellable bgjob ui template */
  const guint *jid = dt_control_backgroundjobs_create(darktable.control, 0, message );
  dt_control_backgroundjobs_set_cancellable(darktable.control, jid, job);
  const dt_control_t *control = darktable.control;

  double fraction=0;
#ifdef _OPENMP
  // limit this to num threads = num full buffers - 1 (keep one for darkroom mode)
  // use min of user request and mipmap cache entries
  const int full_entries = dt_conf_get_int ("parallel_export");
  // GCC won't accept that this variable is used in a macro, considers
  // it set but not used, which makes for instance Fedora break.
  const __attribute__((__unused__)) int num_threads = MAX(1, MIN(full_entries, 8));
#if !defined(__SUNOS__) && !defined(__NetBSD__) && !defined(__WIN32__)
  #pragma omp parallel default(none) private(imgid) shared(control, fraction, w, h, stderr, mformat, mstorage, t, sdata, job, jid, darktable, settings) num_threads(num_threads) if(num_threads > 1)
#else
  #pragma omp parallel private(imgid) shared(control, fraction, w, h, mformat, mstorage, t, sdata, job, jid, darktable, settings) num_threads(num_threads) if(num_threads > 1)
#endif
  {
#endif
    // get a thread-safe fdata struct (one jpeg struct per thread etc):
    dt_imageio_module_data_t *fdata = mformat->get_params(mformat);
    fdata->max_width = settings->max_width;
    fdata->max_height = settings->max_height;
    fdata->max_width = (w!=0 && fdata->max_width >w)?w:fdata->max_width;
    fdata->max_height = (h!=0 && fdata->max_height >h)?h:fdata->max_height;
    strcpy(fdata->style,settings->style);
    int num = 0;
    // Invariant: the tagid for 'darktable|changed' will not change while this function runs. Is this a sensible assumption?
    guint tagid = 0,
          etagid = 0;
    dt_tag_new("darktable|changed",&tagid);
    dt_tag_new("darktable|exported",&etagid);

    while(t && dt_control_job_get_state(job) != DT_JOB_STATE_CANCELLED)
    {
#ifdef _OPENMP
      #pragma omp critical
#endif
      {
        if(!t)
          imgid = 0;
        else
        {
          imgid = GPOINTER_TO_INT(t->data);
          t = g_list_delete_link(t, t);
          num = total - g_list_length(t);
        }
      }
      // remove 'changed' tag from image
      dt_tag_detach(tagid, imgid);
      // make sure the 'exported' tag is set on the image
      dt_tag_attach(etagid, imgid);
      // check if image still exists:
      char imgfilename[DT_MAX_PATH_LEN];
      const dt_image_t *image = dt_image_cache_read_get(darktable.image_cache, (int32_t)imgid);
      if(image)
      {
        gboolean from_cache = TRUE;
        dt_image_full_path(image->id, imgfilename, DT_MAX_PATH_LEN, &from_cache);
        if(!g_file_test(imgfilename, G_FILE_TEST_IS_REGULAR))
        {
          dt_control_log(_("image `%s' is currently unavailable"), image->filename);
          fprintf(stderr, _("image `%s' is currently unavailable"), imgfilename);
          // dt_image_remove(imgid);
          dt_image_cache_read_release(darktable.image_cache, image);
        }
        else
        {
          dt_image_cache_read_release(darktable.image_cache, image);
          mstorage->store(mstorage,sdata, imgid, mformat, fdata, num, total, settings->high_quality);
        }
      }
#ifdef _OPENMP
      #pragma omp critical
#endif
      {
        fraction+=1.0/total;
	if(fraction > 1.0) fraction = 1.0;
        dt_control_backgroundjobs_progress(control, jid, fraction);
      }
    }
#ifdef _OPENMP
    #pragma omp barrier
    #pragma omp master
#endif
    {
      dt_control_backgroundjobs_destroy(control, jid);
      if(mstorage->finalize_store) mstorage->finalize_store(mstorage, sdata);
      mstorage->free_params(mstorage, sdata);
    }
    // all threads free their fdata
    mformat->free_params (mformat, fdata);
#ifdef _OPENMP
  }
#endif
  g_free(t1->data);
  return 0;
}


void dt_control_export(GList *imgid_list,int max_width, int max_height, int format_index, int storage_index, gboolean high_quality,char *style)
{
  dt_job_t job;
  dt_control_job_init(&job, "export");
  job.execute = &dt_control_export_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job.param;
  t->index = imgid_list;
  dt_control_export_t *data = (dt_control_export_t*)malloc(sizeof(dt_control_export_t));
  data->max_width = max_width;
  data->max_height = max_height;
  data->format_index = format_index;
  data->storage_index = storage_index;
  data->high_quality = high_quality;
  g_strlcpy(data->style,style,sizeof(data->style));
  t->data = data;
  dt_control_signal_raise(darktable.signals,DT_SIGNAL_IMAGE_EXPORT_MULTIPLE,t);
  dt_control_add_job(darktable.control, &job);
}

#if GLIB_CHECK_VERSION (2, 26, 0)
int32_t dt_control_time_offset_job_run(dt_job_t *job)
{
  dt_control_image_enumerator_t *t1 = (dt_control_image_enumerator_t *)job->param;
  uint32_t cntr = 0;
  double fraction = 0.0;
  GList *t = t1->index;
  const long int offset = ((dt_control_time_offset_t*)t1->data)->offset;
  guint *jid = NULL;
  char message[512]= {0};

  /* do we have any selected images and is offset != 0 */
  if(!t || offset == 0)
  {
    g_free(t1->data);
    return 1;
  }

  int total = g_list_length(t);

  if(total > 1)
  {
    snprintf(message, 512, ngettext ("adding time offset to %d image", "adding time offset to %d images", total), total );
    jid = (guint *)dt_control_backgroundjobs_create(darktable.control, 0, message);
  }

  /* go thru each selected image and update datetime_taken */
  do
  {
    int imgid = GPOINTER_TO_INT(t->data);

    dt_image_add_time_offset(imgid, offset);
    cntr++;

    if (jid)
    {
      fraction = MAX(fraction, (1.0*cntr)/total);
      dt_control_backgroundjobs_progress(darktable.control, jid, fraction);
    }
  }
  while ((t = g_list_next(t)) != NULL);

  dt_control_log(_("added time offset to %d image(s)"), cntr);

  if (jid)
    dt_control_backgroundjobs_destroy(darktable.control, jid);

  g_free(t1->data);
  return 0;
}

void dt_control_time_offset_job_init(dt_job_t *job, const long int offset, int imgid)
{
  dt_control_job_init(job, "time offset");
  job->execute = &dt_control_time_offset_job_run;
  dt_control_image_enumerator_t *t = (dt_control_image_enumerator_t *)job->param;
  if (imgid != -1)
    t->index = g_list_append(t->index, GINT_TO_POINTER(imgid));
  else
    dt_control_image_enumerator_job_selected_init(t);

  dt_control_time_offset_t *data = (dt_control_time_offset_t*)malloc(sizeof(dt_control_time_offset_t));
  data->offset = offset;
  t->data = data;
}

void dt_control_time_offset(const long int offset, int imgid)
{
  dt_job_t j;
  dt_control_time_offset_job_init(&j, offset, imgid);
  dt_control_add_job(darktable.control, &j);
}
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
