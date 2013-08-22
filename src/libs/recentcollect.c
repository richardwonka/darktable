/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.

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
#include "control/conf.h"
#include "control/signal.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "dtgtk/button.h"
#include "libs/lib.h"
#include "libs/collect.h"

#include <gdk/gdkkeysyms.h>

DT_MODULE(1)

/** this module stores recently used image collection queries and displays
  * them as one-click buttons to the user. */

#define NUM_LINES 10

typedef struct dt_lib_recentcollect_item_t
{
  GtkWidget *button;
}
dt_lib_recentcollect_item_t;

typedef struct dt_lib_recentcollect_t
{
  // 1st is always most recently used entry (buttons stay fixed)
  dt_lib_recentcollect_item_t item[NUM_LINES];
  int inited;
}
dt_lib_recentcollect_t;

const char*
name ()
{
  return _("recently used collections");
}

uint32_t
views()
{
  return DT_VIEW_LIGHTTABLE | DT_VIEW_MAP;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_LEFT_CENTER;
}

int
position ()
{
  return 350;
}

static gboolean
_goto_previous(GtkAccelGroup *accel_group, GObject *acceleratable,
                           guint keyval, GdkModifierType modifier,
                           gpointer data)
{
  gchar *line = dt_conf_get_string("plugins/lighttable/recentcollect/line1");
  if(line)
  {
    dt_collection_deserialize(line);
    g_free(line);
  }
  return TRUE;
}

void init_key_accels(dt_lib_module_t *self)
{
  dt_accel_register_lib(self, NC_("accel", "jump back to previous collection"),
                        GDK_KEY_k, GDK_CONTROL_MASK);
}

void connect_key_accels(dt_lib_module_t *self)
{
  GClosure *closure = g_cclosure_new(G_CALLBACK(_goto_previous), (gpointer)self, NULL);
  dt_accel_connect_lib(self, "jump back to previous collection", closure);
}


static void
pretty_print(char *buf, char *out)
{
  if(!buf || buf[0] == '\0') return;
  int num_rules = 0;
  char str[400] = {0};
  int mode, item;
  sscanf(buf, "%d", &num_rules);
  while(buf[0] != ':') buf++;
  buf++;
  for(int k=0; k<num_rules; k++)
  {
    sscanf(buf, "%d:%d:%[^$]", &mode, &item, str);
    str[399] = '$';

    if(k > 0) switch(mode)
      {
        case DT_LIB_COLLECT_MODE_AND:
          out += sprintf(out, _(" and "));
          break;
        case DT_LIB_COLLECT_MODE_OR:
          out += sprintf(out, _(" or "));
          break;
        default: //case DT_LIB_COLLECT_MODE_AND_NOT:
          out += sprintf(out, _(" but not "));
          break;
      }
    int i = 0;
    while(str[i] != '$') i++;
    str[i] = '\0';

    out += sprintf(out, "%s %s", _(dt_lib_collect_string[item]), item == 0 ? dt_image_film_roll_name(str) : str);
    while(buf[0] != '$' && buf[0] != '\0') buf++;
    buf++;
  }
}

static void
button_pressed (GtkButton *button, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_recentcollect_t *d = (dt_lib_recentcollect_t *) self->data;

  // deserialize this button's preset
  int n = -1;
  for(int k=0; k<NUM_LINES; k++)
  {
    if(button == GTK_BUTTON(d->item[k].button))
    {
      n = k;
      break;
    }
  }
  if(n < 0) return;
  char confname[200];
  snprintf(confname, 200, "plugins/lighttable/recentcollect/line%1d", n);
  gchar *line = dt_conf_get_string(confname);
  if(line)
  {
    dt_collection_deserialize(line);
    g_free(line);
    // position will be updated when the list of recent collections is.
    // that way it'll also catch cases when this is triggered by a signal,
    // not only our button press here.
  }
}

static void _lib_recentcollection_updated(gpointer instance, gpointer user_data)
{
  dt_lib_module_t *self =(dt_lib_module_t *)user_data;
  dt_lib_recentcollect_t *d = (dt_lib_recentcollect_t *)self->data;
  // serialize, check for recently used
  char confname[200];
  const int bufsize = 4096;
  char buf[bufsize];
  if(dt_collection_serialize(buf, bufsize)) return;

  // is the current position, i.e. the one to be stored with the old collection (pos0, pos1-to-be)
  uint32_t curr_pos = 0;
  if(d->inited)
  {
    curr_pos = dt_view_lighttable_get_position(darktable.view_manager);
    dt_conf_set_int("plugins/lighttable/recentcollect/pos0", curr_pos);
  }
  else
  {
    curr_pos = dt_conf_get_int("plugins/lighttable/recentcollect/pos0");
    d->inited = 1;
  }
  uint32_t new_pos = 0;

  int n = -1;
  for(int k=0; k<CLAMPS(dt_conf_get_int("plugins/lighttable/recentcollect/num_items"), 0, NUM_LINES); k++)
  {
    // is it already in the current list?
    snprintf(confname, 200, "plugins/lighttable/recentcollect/line%1d", k);
    gchar *line = dt_conf_get_string(confname);
    if(!line) continue;
    if(!strcmp(line, buf))
    {
      snprintf(confname, 200, "plugins/lighttable/recentcollect/pos%1d", k);
      new_pos = dt_conf_get_int(confname);
      n = k;
      break;
    }
    g_free(line);
  }
  if(n < 0)
  {
    const int num_items = CLAMPS(dt_conf_get_int("plugins/lighttable/recentcollect/num_items"), 0, NUM_LINES);
    if(num_items < NUM_LINES)
    {
      // new, unused entry
      n = num_items;
      dt_conf_set_int("plugins/lighttable/recentcollect/num_items", num_items + 1);
    }
    else
    {
      // kill least recently used entry:
      n = num_items - 1;
    }
  }
  if(n >= 0 && n < NUM_LINES)
  {
    // sort n to the top
    for(int k=n; k>0; k--)
    {
      snprintf(confname, 200, "plugins/lighttable/recentcollect/line%1d", k-1);
      gchar *line1 = dt_conf_get_string(confname);
      snprintf(confname, 200, "plugins/lighttable/recentcollect/pos%1d", k-1);
      uint32_t pos1 = dt_conf_get_int(confname);
      if(line1 && line1[0] != '\0')
      {
        snprintf(confname, 200, "plugins/lighttable/recentcollect/line%1d", k);
        dt_conf_set_string(confname, line1);
        snprintf(confname, 200, "plugins/lighttable/recentcollect/pos%1d", k);
        dt_conf_set_int(confname, pos1);
      }
      g_free(line1);
    }
    dt_conf_set_string("plugins/lighttable/recentcollect/line0", buf);
    dt_conf_set_int("plugins/lighttable/recentcollect/pos0", new_pos);
  }
  // update button descriptions:
  for(int k=0; k<NUM_LINES; k++)
  {
    char str[200] = {0};
    char str_cut[200] = {0};
    char str_pretty[200] = {0};

    snprintf(confname, 200, "plugins/lighttable/recentcollect/line%1d", k);
    gchar *buf = dt_conf_get_string(confname);
    if(buf && buf[0] != '\0')
    {
      pretty_print(buf, str);
      g_free(buf);
    }
    g_object_set(G_OBJECT(d->item[k].button), "tooltip-text", str, (char *)NULL);
    const int cut = 45;
    if (g_utf8_strlen(str, -1) > cut)
    {
      g_utf8_strncpy(str_cut, str, cut);
      snprintf(str_pretty, 200, "%s...", str_cut);
      gtk_button_set_label(GTK_BUTTON(d->item[k].button), str_pretty);
    }
    else
    {
      gtk_button_set_label(GTK_BUTTON(d->item[k].button), str);
    }
    gtk_widget_set_no_show_all(d->item[k].button, TRUE);
    gtk_widget_set_visible(d->item[k].button, FALSE);
  }
  for(int k=0; k<CLAMPS(dt_conf_get_int("plugins/lighttable/recentcollect/num_items"), 0, NUM_LINES); k++)
  {
    gtk_widget_set_no_show_all(d->item[k].button, FALSE);
    gtk_widget_set_visible(d->item[k].button, TRUE);
  }
  dt_view_lighttable_set_position(darktable.view_manager, new_pos);
}

void
gui_reset (dt_lib_module_t *self)
{
  dt_conf_set_int("plugins/lighttable/recentcollect/num_items", 0);
  char confname[200];
  for(int k=0; k<NUM_LINES; k++)
  {
    snprintf(confname, 200, "plugins/lighttable/recentcollect/line%1d", k);
    dt_conf_set_string(confname, "");
    snprintf(confname, 200, "plugins/lighttable/recentcollect/pos%1d", k);
    dt_conf_set_int(confname, 0);
  }
  _lib_recentcollection_updated(NULL,self);
}

void
gui_init (dt_lib_module_t *self)
{
  dt_lib_recentcollect_t *d = (dt_lib_recentcollect_t *)malloc(sizeof(dt_lib_recentcollect_t));
  memset(d,0,sizeof(dt_lib_recentcollect_t));
  self->data = (void *)d;
  self->widget = gtk_vbox_new(FALSE, 0);
  d->inited = 0;

  // add buttons in the list, set them all to invisible
  for(int k=0; k<NUM_LINES; k++)
  {
    d->item[k].button = dtgtk_button_new(NULL, CPF_STYLE_FLAT);
    gtk_box_pack_start(GTK_BOX(self->widget), d->item[k].button, FALSE, TRUE, 0);
    g_signal_connect(G_OBJECT(d->item[k].button), "clicked", G_CALLBACK(button_pressed), (gpointer)self);
    gtk_widget_set_no_show_all(d->item[k].button, TRUE);
    gtk_widget_set_visible(d->item[k].button, FALSE);
  }
  _lib_recentcollection_updated(NULL,self);

  /* connect collection changed signal */
  dt_control_signal_connect(darktable.signals, DT_SIGNAL_COLLECTION_CHANGED,
                            G_CALLBACK(_lib_recentcollection_updated),
                            (gpointer)self);
}

void
gui_cleanup (dt_lib_module_t *self)
{
  uint32_t curr_pos = dt_view_lighttable_get_position(darktable.view_manager);
  dt_conf_set_int("plugins/lighttable/recentcollect/pos0", curr_pos);
  dt_control_signal_disconnect(darktable.signals, G_CALLBACK(_lib_recentcollection_updated), self);
  free(self->data);
  self->data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
