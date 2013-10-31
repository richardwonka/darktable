/*
 This code is taken from http://git.gnome.org/browse/gobject-introspection/tree/giscanner/grealpath.h .
 According to http://git.gnome.org/browse/gobject-introspection/tree/COPYING it's licensed under the LGPLv2+.
*/

#ifndef __G_REALPATH_H__
#define __G_REALPATH_H__

#include <stdlib.h>

/**
 * g_realpath:
 *
 * this should be a) filled in for win32 and b) put in glib...
 */

static inline gchar*
g_realpath (const char *path)
{
#ifndef _WIN32
#ifndef PATH_MAX
#define PATH_MAX 4096
#endif
  char buffer [PATH_MAX];
  if (realpath(path, buffer))
    return g_strdup(buffer);
  else
    return NULL;
#else
  char *buffer;
  char dummy;
  int rc, len;

  rc = GetFullPathNameA(path, 1, &dummy, NULL);

  if (rc == 0)
  {
    /* Weird failure, so just return the input path as such */
    return g_strdup(path);
  }

  len = rc + 1;
  buffer = g_malloc(len);

  rc = GetFullPathNameA(path, len, buffer, NULL);

  if (rc == 0 || rc > len)
  {
    /* Weird failure again */
    g_free(buffer);
    return g_strdup(path);
  }

  return buffer;
#endif
}

#endif

// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
