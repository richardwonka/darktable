/*
   This file is part of darktable,
   copyright (c) 2012 Jeremy Rosen

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
#ifndef LUA_LUA_H
#define LUA_LUA_H

/* this file can safely be included when lua is disabled */

#ifdef USE_LUA
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <lautoc.h>

/**
  (0,+1)
  find or create the global darktable module table and push it on the stack
  */
int dt_lua_push_darktable_lib(lua_State* L);

/**
  (-1,+1)
  check that the top of the stack is a table, creates or find a subtable named "name",
  adds it on top of the stack, and remove the previous table

  used to easily do a tree organisation of objects
*/
void dt_lua_goto_subtable(lua_State *L,const char* sub_name);


void dt_lua_init_lock();
void dt_lua_lock();
void dt_lua_unlock();

#define dt_lua_debug_stack(L) dt_lua_debug_stack_internal(L,__FUNCTION__,__LINE__)
void dt_lua_debug_stack_internal(lua_State *L, const char* function, int line);
#define dt_lua_debug_table(L,index) dt_lua_debug_table_internal(L,index,__FUNCTION__,__LINE__)
void dt_lua_debug_table_internal(lua_State * L,int t,const char* function,int line);

#else
/* defines to easily have a few lua types when lua is not available */
typedef int lua_State ;
typedef int (*lua_CFunction)(lua_State *L);
typedef int luaA_Type;
#define LUAA_INVALID_TYPE -1
#endif



#endif
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;

