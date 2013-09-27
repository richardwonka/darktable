/*
    This file is part of darktable,
    copyright (c) 2009--2010 johannes hanika.

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
#ifndef DT_USER_CONFIG_H
#define DT_USER_CONFIG_H

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "common/darktable.h"
#include "common/file_location.h"

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <glib.h>
#include <glib/gprintf.h>

typedef struct dt_conf_t
{
  dt_pthread_mutex_t mutex;
  char filename[DT_MAX_PATH_LEN];
  GHashTable *table;
  GHashTable *defaults;
  GHashTable *override_entries;
}
dt_conf_t;

typedef struct dt_conf_string_entry_t
{
  char *key;
  char *value;
}
dt_conf_string_entry_t;

typedef struct dt_conf_dreggn_t
{
  GSList *result;
  const char *match;
}
dt_conf_dreggn_t;

/** return slot for this variable or newly allocated slot. */
static inline char *dt_conf_get_var(const char *name)
{
  char *str = (char *)g_hash_table_lookup(darktable.conf->override_entries, name);
  if(str) return str;

  str = (char *)g_hash_table_lookup(darktable.conf->table, name);
  if(str) return str;

  // not found, try defaults
  str = (char *)g_hash_table_lookup(darktable.conf->defaults, name);
  if(str)
  {
    g_hash_table_insert(darktable.conf->table, g_strdup(name), g_strdup(str));
    // and try again:
    return dt_conf_get_var(name);
  }

  // still no luck? insert garbage:
  char *garbage = (char *)g_malloc(sizeof(int32_t));
  memset(garbage, 0, sizeof(int32_t));
  g_hash_table_insert(darktable.conf->table, g_strdup(name), garbage);
  return garbage;
}

/** return if key/value is still the one passed on commandline. */
static inline int dt_conf_is_still_overridden(const char *name, const char *value)
{
  char *over = (char *)g_hash_table_lookup(darktable.conf->override_entries, name);
  return (over && !strcmp(value, over));
}

static inline void dt_conf_set_int(const char *name, int val)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  char *str = g_strdup_printf("%d", val);
  if(!dt_conf_is_still_overridden(name, str))
    g_hash_table_insert(darktable.conf->table, g_strdup(name), str);
  else
    g_free(str);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
}

static inline void dt_conf_set_int64(const char *name, int64_t val)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  char *str = g_strdup_printf("%" PRId64, val);
  if(!dt_conf_is_still_overridden(name, str))
    g_hash_table_insert(darktable.conf->table, g_strdup(name), str);
  else
    g_free(str);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
}

static inline void dt_conf_set_float(const char *name, float val)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  char *str = (char *)g_malloc(G_ASCII_DTOSTR_BUF_SIZE);
  g_ascii_dtostr(str, G_ASCII_DTOSTR_BUF_SIZE, val);
  if(!dt_conf_is_still_overridden(name, str))
    g_hash_table_insert(darktable.conf->table, g_strdup(name), str);
  else
    g_free(str);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
}

static inline void dt_conf_set_bool(const char *name, int val)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  char *str = g_strdup_printf("%s", val ? "TRUE" : "FALSE");
  if(!dt_conf_is_still_overridden(name, str))
    g_hash_table_insert(darktable.conf->table, g_strdup(name), str);
  else
    g_free(str);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
}

static inline void dt_conf_set_string(const char *name, const char *val)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  if(!dt_conf_is_still_overridden(name, val))
    g_hash_table_insert(darktable.conf->table, g_strdup(name), g_strdup(val));
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
}

static inline int dt_conf_get_int(const char *name)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const char *str = dt_conf_get_var(name);
  const int val = atol(str);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return val;
}

static inline int64_t dt_conf_get_int64(const char *name)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const char *str = dt_conf_get_var(name);
  const int64_t val = g_ascii_strtoll(str, NULL, 10);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return val;
}

static inline float dt_conf_get_float(const char *name)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const char *str = dt_conf_get_var(name);
  const float val = g_ascii_strtod(str, NULL);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return val;
}

static inline int dt_conf_get_bool(const char *name)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const char *str = dt_conf_get_var(name);
  const int val = (str[0] == 'T') || (str[0] == 't');
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return val;
}

static inline gchar *dt_conf_get_string(const char *name)
{
  dt_pthread_mutex_lock(&darktable.conf->mutex);
  const char *str = dt_conf_get_var(name);
  dt_pthread_mutex_unlock(&darktable.conf->mutex);
  return g_strdup(str);
}

static inline void dt_conf_init(dt_conf_t *cf, const char *filename, GSList *override_entries)
{
  cf->table            = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  cf->defaults         = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  cf->override_entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
  dt_pthread_mutex_init(&darktable.conf->mutex, NULL);
  FILE *f = 0;
  char line[1024];
  int read = 0;
  int defaults = 0;
  for(int i=0; i<2; i++)
  {
    // TODO: read default darktablerc into ->defaults and other into ->table!
    if(!i)
    {
      snprintf(darktable.conf->filename, DT_MAX_PATH_LEN, "%s", filename);
      f = fopen(filename, "rb");
      if(!f)
      {
        // remember we init to default rc and try again
        defaults = 1;
        continue;
      }
    }
    if(i)
    {
      char buf[DT_MAX_PATH_LEN], defaultrc[DT_MAX_PATH_LEN];
      dt_loc_get_datadir(buf, DT_MAX_PATH_LEN);
      snprintf(defaultrc, DT_MAX_PATH_LEN, "%s/darktablerc", buf);
      f = fopen(defaultrc, "rb");
    }
    if(!f) return;
    while(!feof(f))
    {
      read = fscanf(f, "%[^\n]\n", line);
      if(read > 0)
      {
        char *c = line;
        while(*c != '=' && c < line + strlen(line)) c++;
        if(*c == '=')
        {
          *c = '\0';
          if(i)
            g_hash_table_insert(darktable.conf->defaults, g_strdup(line), g_strdup(c+1));
          if(!i || defaults)
            g_hash_table_insert(darktable.conf->table, g_strdup(line), g_strdup(c+1));
        }
      }
    }
    fclose(f);
  }
  if(defaults) dt_configure_defaults();

  if(override_entries)
  {
    GSList *p = override_entries;
    while(p)
    {
      dt_conf_string_entry_t *entry = (dt_conf_string_entry_t*)p->data;
      g_hash_table_insert(darktable.conf->override_entries, entry->key, entry->value);
      p = g_slist_next(p);
    }
  }

  return;
}

static void _conf_print(char *key, char *val, FILE *f)
{
  fprintf(f, "%s=%s\n", key, val);
}

static inline void dt_conf_cleanup(dt_conf_t *cf)
{
  FILE *f = fopen(cf->filename, "wb");
  if(f)
  {
    g_hash_table_foreach(cf->table, (GHFunc)_conf_print, f);
    fclose(f);
  }
  g_hash_table_unref(cf->table);
  g_hash_table_unref(cf->defaults);
  g_hash_table_unref(cf->override_entries);
  dt_pthread_mutex_destroy(&darktable.conf->mutex);
}

/** check if key exists, return 1 if lookup successed, 0 if failed..*/
static inline int dt_conf_key_exists (const char *key)
{
  dt_pthread_mutex_lock (&darktable.conf->mutex);
  const int res = (g_hash_table_lookup(darktable.conf->table, key) != NULL) || (g_hash_table_lookup(darktable.conf->override_entries, key) != NULL);
  dt_pthread_mutex_unlock (&darktable.conf->mutex);
  return res;
}

static void _conf_add(char *key, char *val, dt_conf_dreggn_t *d)
{
  if(!strcmp(key, d->match))
  {
    dt_conf_string_entry_t *nv = (dt_conf_string_entry_t*)g_malloc (sizeof(dt_conf_string_entry_t));
    nv->key = g_strdup(key);
    nv->value = g_strdup(val);
    d->result = g_slist_append(d->result, nv);
  }
}

/** get all strings in */
static inline GSList *dt_conf_all_string_entries (const char *dir)
{
  dt_pthread_mutex_lock (&darktable.conf->mutex);
  GSList *result = NULL;
  dt_conf_dreggn_t d;
  d.result = result;
  d.match = dir;
  g_hash_table_foreach(darktable.conf->table, (GHFunc)_conf_add, &d);
  dt_pthread_mutex_unlock (&darktable.conf->mutex);
  return result;
}


#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
