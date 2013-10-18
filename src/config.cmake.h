#pragma once

// CMake uses config.cmake.h to generate config.h within the build folder.
#ifndef DARKTABLE_CONFIG_H
#define DARKTABLE_CONFIG_H

#define GETTEXT_PACKAGE "darktable"
#define DARKTABLE_LOCALEDIR "@LOCALE_DIR@"
#define DARKTABLE_TMPDIR "@TMP_DIR@"
#define DARKTABLE_CACHEDIR "@CACHE_DIR@"

#define PACKAGE_NAME "@CMAKE_PROJECT_NAME@"
#define PACKAGE_VERSION "@PROJECT_VERSION@"
#define PACKAGE_STRING PACKAGE_NAME " " PACKAGE_VERSION
#define PACKAGE_BUGREPORT "darktable-devel@lists.sf.net"

#define DARKTABLE_LIBDIR "@CMAKE_INSTALL_PREFIX@/@LIB_INSTALL@/darktable"
#define DARKTABLE_DATADIR "@CMAKE_INSTALL_PREFIX@/@SHARE_INSTALL@/darktable"

#define SHARED_MODULE_PREFIX "@CMAKE_SHARED_MODULE_PREFIX@"
#define SHARED_MODULE_SUFFIX "@CMAKE_SHARED_MODULE_SUFFIX@"

#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
