/*
    This file is part of darktable,
    copyright (c) 2011 Henrik Andersson.

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
#include "common/debug.h"
#include "control/control.h"
#include "control/conf.h"
#include "common/image_cache.h"
#include "develop/develop.h"
#include "libs/lib.h"
#include "gui/gtk.h"

DT_MODULE(1)


typedef struct dt_lib_darktable_t
{
  cairo_surface_t *image;
}
dt_lib_darktable_t;


/* expose function for darktable module */
static gboolean _lib_darktable_expose_callback(GtkWidget *widget, GdkEventExpose *event, gpointer user_data);
/* button press callback */
static gboolean _lib_darktable_button_press_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data);
/* show the about dialog */
static void _lib_darktable_show_about_dialog();

const char* name()
{
  return _("darktable");
}

uint32_t views()
{
  return DT_VIEW_ALL;
}

uint32_t container()
{
  return DT_UI_CONTAINER_PANEL_TOP_LEFT;
}

int expandable()
{
  return 0;
}

int position()
{
  return 1001;
}


void gui_init(dt_lib_module_t *self)
{
  char filename[DT_MAX_PATH_LEN];
  char datadir[DT_MAX_PATH_LEN];
  /* initialize ui widgets */
  dt_lib_darktable_t *d = (dt_lib_darktable_t *)g_malloc(sizeof(dt_lib_darktable_t));
  self->data = (void *)d;
  memset(d,0,sizeof(dt_lib_darktable_t));

  /* create drawing area */
  self->widget = gtk_event_box_new();

  /* connect callbacks */
  g_signal_connect (G_OBJECT (self->widget), "expose-event",
                    G_CALLBACK (_lib_darktable_expose_callback), self);
  g_signal_connect (G_OBJECT (self->widget), "button-press-event",
                    G_CALLBACK (_lib_darktable_button_press_callback), self);

  /* set size of drawing area */
  gtk_widget_set_size_request(self->widget, 220, 48);

  /* create a cairo surface of dt icon */
  dt_loc_get_datadir(datadir, DT_MAX_PATH_LEN);
  snprintf(filename, DT_MAX_PATH_LEN, "%s/pixmaps/idbutton.png", datadir);
  d->image = cairo_image_surface_create_from_png(filename);

}

void gui_cleanup(dt_lib_module_t *self)
{
  dt_lib_darktable_t *d = (dt_lib_darktable_t *)self->data;
  cairo_surface_destroy(d->image);
  g_free(self->data);
  self->data = NULL;
}



static gboolean _lib_darktable_expose_callback(GtkWidget *widget, GdkEventExpose *event, gpointer user_data)
{
  dt_lib_module_t *self = (dt_lib_module_t *)user_data;
  dt_lib_darktable_t *d = (dt_lib_darktable_t *)self->data;

  /* get the current style */
  GtkStyle *style=gtk_rc_get_style_by_paths(gtk_settings_get_default(), NULL,"GtkWidget", GTK_TYPE_WIDGET);
  if(!style) style = gtk_rc_get_style(widget);

  cairo_t *cr = gdk_cairo_create(gtk_widget_get_window(widget));

  /* fill background */
  cairo_set_source_rgb(cr, style->bg[0].red/65535.0, style->bg[0].green/65535.0, style->bg[0].blue/65535.0);
  cairo_paint(cr);

  /* paint icon image */
  cairo_set_source_surface(cr, d->image, 0, 7);
  cairo_rectangle(cr,0,0,48,48);
  cairo_fill(cr);


  /* create a pango layout and print fancy  name/version string */
  PangoLayout *layout;
  layout = gtk_widget_create_pango_layout (widget,NULL);
  pango_font_description_set_weight (style->font_desc, PANGO_WEIGHT_BOLD);
  pango_font_description_set_absolute_size (style->font_desc, 25 * PANGO_SCALE);
  pango_layout_set_font_description (layout,style->font_desc);

  pango_layout_set_text (layout,PACKAGE_NAME,-1);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
  cairo_move_to (cr, 42.0, 5.0);
  pango_cairo_show_layout (cr, layout);

  /* print version */
  pango_font_description_set_absolute_size (style->font_desc, 10 * PANGO_SCALE);
  pango_layout_set_font_description (layout,style->font_desc);
  pango_layout_set_text (layout,PACKAGE_VERSION,-1);
  cairo_move_to (cr, 44.0, 30.0);
  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.3);
  pango_cairo_show_layout (cr, layout);

  /* cleanup */
  g_object_unref (layout);
  cairo_destroy(cr);

  return TRUE;
}

static gboolean _lib_darktable_button_press_callback(GtkWidget *widget, GdkEventButton *event, gpointer user_data)
{
  /* show about box */
  _lib_darktable_show_about_dialog();
  return TRUE;
}

static void _lib_darktable_show_about_dialog()
{
  GtkWidget *dialog = gtk_about_dialog_new();
  gtk_about_dialog_set_name(GTK_ABOUT_DIALOG(dialog), PACKAGE_NAME);
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), PACKAGE_VERSION);
  gtk_about_dialog_set_copyright(GTK_ABOUT_DIALOG(dialog), "copyright (c) the authors 2009-2013");
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog), _("organize and develop images from digital cameras"));
  gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog), "http://www.darktable.org/");
  gtk_about_dialog_set_logo_icon_name(GTK_ABOUT_DIALOG(dialog), "darktable");
  const char *authors[] =
  {
    _("* developers *"),
    "Henrik Andersson",
    "Johannes Hanika",
    "Tobias Ellinghaus",
    "Ulrich Pegelow",
    "",
    _("* ubuntu packaging, color management, video tutorials *"),
    "Pascal de Bruijn",
    "",
    _("* OpenCL pipeline: *"),
    "Ulrich Pegelow",
    "",
    _("* networking, battle testing, translation expert *"),
    "Alexandre Prokoudine",
    "",
    _("* contributors *"),
    "Pascal Obry",
    "Pascal de Bruijn",
    "Jean-Sébastien Pédron",
    "Jose Carlos Garcia Sogo",
    "parafin",
    "Richard Levitte",
    "Aldric Renaudin",
    "Jérémy Rosen",
    "Simon Spannagel",
    "Dennis Gnad",
    "Eckhart Pedersen",
    "Michal Babej",
    "Roman Lebedev",
    "Gaspard Jankowiak",
    "pmjdebruijn",
    "Olivier",
    "Moritz Lipp",
    "Ivan Tarozzi",
    "hal",
    "Edouard Gomez",
    "Stuart Henderson",
    "Olivier Tribout",
    "Guilherme Brondani Torri",
    "Wolfgang Kuehnel",
    "Richard Tollerton",
    "James C. McPherson",
    "Jesper Pedersen",
    "christte",
    "Alexandre Prokoudine",
// one committers:
    "Wolfgang Goetz",
    "sthen",
    "sjjh",
    "Roland Riegel",
    "Julian J. M",
    "Jon Leighton",
    "franz",
    "Diego Segura",
    "Chris Mason",
    "Ari Makela",
    "And all those of you that made previous releases possible",
    NULL
  };
  gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dialog), authors);

  gtk_about_dialog_set_translator_credits(GTK_ABOUT_DIALOG(dialog), _("translator-credits"));
  gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(dt_ui_main_window(darktable.gui->ui)));
  gtk_dialog_run(GTK_DIALOG (dialog));
  gtk_widget_destroy(dialog);

}
// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
