/*
    This file is part of darktable,
    copyright (c) 2010-2011 Henrik Andersson, Tobias Ellinghaus.

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

/* getpwnam_r availibility check */
#if defined __APPLE__ || defined _POSIX_C_SOURCE >= 1 || \
    defined _XOPEN_SOURCE || defined _BSD_SOURCE || defined _SVID_SOURCE || \
    defined _POSIX_SOURCE || defined __DragonFly__ || defined __FreeBSD__ || \
    defined __NetBSD__ || defined __OpenBSD__
#include <pwd.h>
#include <sys/types.h>
#include <unistd.h>
#include "darktable.h"
#endif

#include <sys/stat.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include "utility.h"
#include "file_location.h"

gchar *dt_util_dstrcat(gchar *str,const gchar *format, ... )
{
  va_list args;
  gchar *ns;
  va_start(args, format);
  int clen = str ? strlen(str) : 0;
  int alen = g_vsnprintf(NULL, 0, format, args);
  int nsize = alen + clen + 1;

  /* realloc for new string */
  ns = g_realloc(str, nsize);
  if (str == NULL) ns[0] = '\0';
  va_end(args);

  /* append string */
  va_start(args, format);
  g_vsnprintf(ns+clen, alen+1, format, args);
  va_end(args);

  ns[nsize-1]='\0';

  return ns;
}

guint dt_util_str_occurence(const gchar *haystack,const gchar *needle)
{
  guint o=0;
  if( haystack && needle )
  {
    const gchar *p=haystack;
    if( (p=g_strstr_len(p,strlen(p),needle)) != NULL)
    {
      do
      {
        o++;
      }
      while((p=g_strstr_len((p+1),strlen(p+1),needle)) != NULL);
    }
  }
  return o;
}

gchar* dt_util_str_replace(const gchar* string, const gchar* pattern, const gchar* substitute)
{
  gint occurences = dt_util_str_occurence(string, pattern);
  gchar* nstring;
  if(occurences)
  {
    nstring=g_malloc(strlen(string)+(occurences*strlen(substitute))+1);
    const gchar *pend=string+strlen(string);
    const gchar *s = string, *p = string;
    gchar *np = nstring;
    if((s=g_strstr_len(s,strlen(s),pattern)) != NULL)
    {
      do
      {
        memcpy(np,p,s-p);
        np+=(s-p);
        memcpy(np,substitute,strlen(substitute));
        np+=strlen(substitute);
        p=s+strlen(pattern);
      }
      while((s=g_strstr_len((s+1),strlen(s+1),pattern)) != NULL);
    }
    memcpy(np,p,pend-p);
    np[pend-p]='\0';
  }
  else
    nstring = g_strdup(string); // otherwise it's a hell to decide whether to free this string later.
  return nstring;
}

gchar* dt_util_glist_to_str(const gchar* separator, GList * items, const unsigned int count)
{
  if(count == 0)
    return NULL;

  gchar *result = NULL;

  // add the entries to an char* array
  items = g_list_first(items);
  gchar** strings = g_malloc(sizeof(gchar*) * (count+1));
  if(items != NULL)
  {
    int i = 0;
    do
    {
      strings[i++] = items->data;
    }
    while((items=g_list_next(items)) != NULL);
    strings[i] = NULL;
  }

  // join them into a single string
  result = g_strjoinv(separator, strings);

  // free the GList and the array
  items = g_list_first(items);
  if(items != NULL)
  {
    do
    {
      g_free(items->data);
    }
    while((items=g_list_next(items)) != NULL);
  }
  g_list_free(items);
  if(strings != NULL)
    g_free(strings);

  return result;
}

gchar* dt_util_fix_path(const gchar* path)
{
  if (path == NULL || strlen(path) == 0)
  {
    return NULL;
  }

  gchar* rpath = NULL;

  /* check if path has a prepended tilde */
  if (path[0] == '~')
  {
    int len    = strlen(path);
    char* user = NULL;
    int off    = 1;

    /* if the character after the tilde is not a slash we parse
     * the path until the next slash to extend this part with the
     * home directory of the specified user
     *
     * e.g.: ~foo will be evaluated as the home directory of the
     * user foo */

    if (len > 1 && path[1] != '/')
    {
      while (path[off] != '\0' && path[off] != '/')
      {
        ++off;
      }

      user = g_strndup(path + 1, off - 1);
    }

    gchar* home_path = dt_loc_get_home_dir(user);
    g_free(user);

    if (home_path == NULL)
    {
      return g_strdup(path);
    }

    rpath = g_build_filename(home_path, path + off, NULL);
  }
  else
  {
    rpath = g_strdup(path);
  }

  return rpath;
}

/**
 * dt_utf8_strlcpy:
 * @dest: buffer to fill with characters from @src
 * @src: UTF-8 encoded string
 * @n: size of @dest
 *
 * Like the BSD-standard strlcpy() function, but
 * is careful not to truncate in the middle of a character.
 * The @src string must be valid UTF-8 encoded text.
 * (Use g_utf8_validate() on all text before trying to use UTF-8
 * utility functions with it.)
 *
 * Return value: strlen(src)
 * Implementation by Philip Page, see https://bugzilla.gnome.org/show_bug.cgi?id=520116
 **/
size_t dt_utf8_strlcpy(char *dest, const char *src,size_t n)
{
  register const gchar *s = src;
  while (s - src < n  &&  *s)
  {
    s = g_utf8_next_char(s);
  }

  if (s - src >= n)
  {
    /* We need to truncate; back up one. */
    s = g_utf8_prev_char(s);
    strncpy(dest, src, s - src);
    dest[s - src] = '\0';
    /* Find the full length for return value. */
    while (*s)
    {
      s = g_utf8_next_char(s);
    }
  }
  else
  {
      /* Plenty of room, just copy */
    strncpy(dest, src, s - src);
    dest[s - src] = '\0';
  }
  return s - src;
}

off_t dt_util_get_file_size(const char *filename)
{
#ifdef __WIN32__
  struct _stati64 st;
  if(_stati64(filename, &st) == 0)
    return st.st_size;
#else
  struct stat st;
  if(stat(filename, &st) == 0)
    return st.st_size;
#endif

  return -1;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
