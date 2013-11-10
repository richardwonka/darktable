/*
    This file is part of darktable,
    copyright (c) 2012 tobias ellinghaus.

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

// Original copyright notice from image_to_j2k.c from openjpeg:
/*
 * Copyright (c) 2002-2007, Communications and Remote Sensing Laboratory, Universite catholique de Louvain (UCL), Belgium
 * Copyright (c) 2002-2007, Professor Benoit Macq
 * Copyright (c) 2001-2003, David Janssens
 * Copyright (c) 2002-2003, Yannick Verschueren
 * Copyright (c) 2003-2007, Francois-Olivier Devaux and Antonin Descampe
 * Copyright (c) 2005, Herve Drolon, FreeImage Team
 * Copyright (c) 2006-2007, Parvatha Elangovan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS `AS IS'
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <stdlib.h>
#include <stdio.h>
#include "common/darktable.h"
#include "common/exif.h"
#include "common/imageio_module.h"
#include "common/imageio.h"
#include "control/conf.h"
#include "dtgtk/slider.h"
#include "common/imageio_format.h"

#include <openjpeg.h>

#define CINEMA_24_CS 1302083    /*Codestream length for 24fps*/
#define CINEMA_48_CS 651041     /*Codestream length for 48fps*/
#define COMP_24_CS 1041666      /*Maximum size per color component for 2K & 4K @ 24fps*/
#define COMP_48_CS 520833       /*Maximum size per color component for 2K @ 48fps*/

typedef enum
{
  J2K_CFMT,
  JP2_CFMT
} dt_imageio_j2k_format_t;

// borrowed from blender
#define DOWNSAMPLE_FLOAT_TO_8BIT(_val)  (_val) <= 0.0f ? 0 : ((_val) >= 1.0f ? 255 : (int)(255.0f * (_val)))
#define DOWNSAMPLE_FLOAT_TO_12BIT(_val) (_val) <= 0.0f ? 0 : ((_val) >= 1.0f ? 4095 : (int)(4095.0f * (_val)))
#define DOWNSAMPLE_FLOAT_TO_16BIT(_val) (_val) <= 0.0f ? 0 : ((_val) >= 1.0f ? 65535 : (int)(65535.0f * (_val)))

DT_MODULE(1)

typedef enum
{
  DT_J2K_PRESET_OFF,
  DT_J2K_PRESET_CINEMA2K_24,
  DT_J2K_PRESET_CINEMA2K_48,
  DT_J2K_PRESET_CINEMA4K_24
} dt_imageio_j2k_preset_t;

typedef struct dt_imageio_j2k_t
{
  int max_width, max_height;
  int width, height;
  char style[128];
  int bpp;
  dt_imageio_j2k_format_t format;
  dt_imageio_j2k_preset_t preset;
  int quality;
}
dt_imageio_j2k_t;

typedef struct dt_imageio_j2k_gui_t
{
  GtkToggleButton *jp2, *j2k;
  GtkComboBox *preset;
  GtkDarktableSlider *quality;
}
dt_imageio_j2k_gui_t;

void init(dt_imageio_module_format_t *self)
{
#ifdef USE_LUA
  dt_lua_register_module_member(darktable.lua_state.state,self,dt_imageio_j2k_t,bpp,int);
  luaA_enum(darktable.lua_state.state,dt_imageio_j2k_format_t);
  luaA_enum_value_name(darktable.lua_state.state,dt_imageio_j2k_format_t,J2K_CFMT,"j2k",false);
  luaA_enum_value_name(darktable.lua_state.state,dt_imageio_j2k_format_t,J2K_CFMT,"jp2",false);
  dt_lua_register_module_member(darktable.lua_state.state,self,dt_imageio_j2k_t,format,dt_imageio_j2k_format_t);
  dt_lua_register_module_member(darktable.lua_state.state,self,dt_imageio_j2k_t,quality,int);
  luaA_enum(darktable.lua_state.state,dt_imageio_j2k_preset_t);
  luaA_enum_value_name(darktable.lua_state.state,dt_imageio_j2k_preset_t,DT_J2K_PRESET_OFF,"off",false);
  luaA_enum_value_name(darktable.lua_state.state,dt_imageio_j2k_preset_t,DT_J2K_PRESET_CINEMA2K_24,"cinema2k_24",false);
  luaA_enum_value_name(darktable.lua_state.state,dt_imageio_j2k_preset_t,DT_J2K_PRESET_CINEMA2K_48,"cinema2k_48",false);
  luaA_enum_value_name(darktable.lua_state.state,dt_imageio_j2k_preset_t,DT_J2K_PRESET_CINEMA4K_24,"cinema4k_24",false);
  dt_lua_register_module_member(darktable.lua_state.state,self,dt_imageio_j2k_t,preset,dt_imageio_j2k_preset_t);
#endif
}
void cleanup(dt_imageio_module_format_t *self) {}

/**
sample error callback expecting a FILE* client object
*/
static void error_callback(const char *msg, void *client_data)
{
  FILE *stream = (FILE*)client_data;
  fprintf(stream, "[ERROR] %s", msg);
}
/**
sample warning callback expecting a FILE* client object
*/
static void warning_callback(const char *msg, void *client_data)
{
  FILE *stream = (FILE*)client_data;
  fprintf(stream, "[WARNING] %s", msg);
}
/**
sample debug callback expecting a FILE* client object
*/
static void info_callback(const char *msg, void *client_data)
{
  FILE *stream = (FILE*)client_data;
  fprintf(stream, "[INFO] %s", msg);
}

static int initialise_4K_poc(opj_poc_t *POC, int numres)
{
  POC[0].tile    = 1;
  POC[0].resno0  = 0;
  POC[0].compno0 = 0;
  POC[0].layno1  = 1;
  POC[0].resno1  = numres-1;
  POC[0].compno1 = 3;
  POC[0].prg1    = CPRL;
  POC[1].tile    = 1;
  POC[1].resno0  = numres-1;
  POC[1].compno0 = 0;
  POC[1].layno1  = 1;
  POC[1].resno1  = numres;
  POC[1].compno1 = 3;
  POC[1].prg1    = CPRL;
  return 2;
}

static void cinema_parameters(opj_cparameters_t *parameters)
{
  parameters->tile_size_on = 0;
  parameters->cp_tdx = 1;
  parameters->cp_tdy = 1;

  /*Tile part*/
  parameters->tp_flag = 'C';
  parameters->tp_on = 1;

  /*Tile and Image shall be at (0,0)*/
  parameters->cp_tx0 = 0;
  parameters->cp_ty0 = 0;
  parameters->image_offset_x0 = 0;
  parameters->image_offset_y0 = 0;

  /*Codeblock size= 32*32*/
  parameters->cblockw_init = 32;
  parameters->cblockh_init = 32;
  parameters->csty |= 0x01;

  /*The progression order shall be CPRL*/
  parameters->prog_order = CPRL;

  /* No ROI */
  parameters->roi_compno = -1;

  parameters->subsampling_dx = 1;
  parameters->subsampling_dy = 1;

  /* 9-7 transform */
  parameters->irreversible = 1;
}

static void cinema_setup_encoder(opj_cparameters_t *parameters,opj_image_t *image, float *rates)
{
  int i;
  float temp_rate;

  switch(parameters->cp_cinema)
  {
    case CINEMA2K_24:
    case CINEMA2K_48:
      parameters->cp_rsiz = CINEMA2K;
      if(parameters->numresolution > 6)
      {
        parameters->numresolution = 6;
      }
      if (!((image->comps[0].w == 2048) | (image->comps[0].h == 1080)))
      {
        fprintf(stdout,"Image coordinates %d x %d is not 2K compliant.\nJPEG Digital Cinema Profile-3 "
                "(2K profile) compliance requires that at least one of coordinates match 2048 x 1080\n",
                image->comps[0].w,image->comps[0].h);
        parameters->cp_rsiz = STD_RSIZ;
      }
      break;

    case CINEMA4K_24:
      parameters->cp_rsiz = CINEMA4K;
      if(parameters->numresolution < 1)
      {
        parameters->numresolution = 1;
      }
      else if(parameters->numresolution > 7)
      {
        parameters->numresolution = 7;
      }
      if (!((image->comps[0].w == 4096) | (image->comps[0].h == 2160)))
      {
        fprintf(stdout,"Image coordinates %d x %d is not 4K compliant.\nJPEG Digital Cinema Profile-4"
                "(4K profile) compliance requires that at least one of coordinates match 4096 x 2160\n",
                image->comps[0].w,image->comps[0].h);
        parameters->cp_rsiz = STD_RSIZ;
      }
      parameters->numpocs = initialise_4K_poc(parameters->POC,parameters->numresolution);
      break;
    default:
      break;
  }

  switch(parameters->cp_cinema)
  {
    case CINEMA2K_24:
    case CINEMA4K_24:
      for(i=0; i<parameters->tcp_numlayers; i++)
      {
        temp_rate = 0;
        if(rates[i] == 0)
        {
          parameters->tcp_rates[0] = ((float) (image->numcomps * image->comps[0].w * image->comps[0].h * image->comps[0].prec)) /
                                     (CINEMA_24_CS * 8 * image->comps[0].dx * image->comps[0].dy);
        }
        else
        {
          temp_rate = ((float) (image->numcomps * image->comps[0].w * image->comps[0].h * image->comps[0].prec))/
                      (rates[i] * 8 * image->comps[0].dx * image->comps[0].dy);
          if(temp_rate > CINEMA_24_CS)
          {
            parameters->tcp_rates[i] = ((float) (image->numcomps * image->comps[0].w * image->comps[0].h * image->comps[0].prec))/
                                       (CINEMA_24_CS * 8 * image->comps[0].dx * image->comps[0].dy);
          }
          else
          {
            parameters->tcp_rates[i] = rates[i];
          }
        }
      }
      parameters->max_comp_size = COMP_24_CS;
      break;

    case CINEMA2K_48:
      for(i=0; i<parameters->tcp_numlayers; i++)
      {
        temp_rate = 0;
        if(rates[i] == 0)
        {
          parameters->tcp_rates[0] = ((float) (image->numcomps * image->comps[0].w * image->comps[0].h * image->comps[0].prec))/
                                     (CINEMA_48_CS * 8 * image->comps[0].dx * image->comps[0].dy);
        }
        else
        {
          temp_rate = ((float) (image->numcomps * image->comps[0].w * image->comps[0].h * image->comps[0].prec))/
                      (rates[i] * 8 * image->comps[0].dx * image->comps[0].dy);
          if(temp_rate > CINEMA_48_CS)
          {
            parameters->tcp_rates[0] = ((float) (image->numcomps * image->comps[0].w * image->comps[0].h * image->comps[0].prec))/
                                       (CINEMA_48_CS * 8 * image->comps[0].dx * image->comps[0].dy);
          }
          else
          {
            parameters->tcp_rates[i] = rates[i];
          }
        }
      }
      parameters->max_comp_size = COMP_48_CS;
      break;
    default:
      break;
  }
  parameters->cp_disto_alloc = 1;
}

int write_image (dt_imageio_module_data_t *j2k_tmp, const char *filename, const void *in_tmp, void *exif, int exif_len, int imgid)
{
  const float * in = (const float *)in_tmp;
  dt_imageio_j2k_t * j2k = (dt_imageio_j2k_t*)j2k_tmp;
  opj_cparameters_t parameters;     /* compression parameters */
  float *rates = NULL;
  opj_event_mgr_t event_mgr;        /* event manager */
  opj_image_t *image = NULL;
  int quality = CLAMP(j2k->quality, 1, 100);

  /*
  configure the event callbacks (not required)
  setting of each callback is optional
  */
  memset(&event_mgr, 0, sizeof(opj_event_mgr_t));
  event_mgr.error_handler = error_callback;
  event_mgr.warning_handler = warning_callback;
  event_mgr.info_handler = info_callback;

  /* set encoding parameters to default values */
  opj_set_default_encoder_parameters(&parameters);

  /* compression ratio */
  /* invert range, from 10-100, 100-1
  * where jpeg see's 1 and highest quality (lossless) and 100 is very low quality*/
  parameters.tcp_rates[0] = 100 - quality + 1;

  parameters.tcp_numlayers = 1; /* only one resolution */
  parameters.cp_disto_alloc = 1;
  parameters.cp_rsiz = STD_RSIZ;

  parameters.cod_format = j2k->format;
  parameters.cp_cinema = j2k->preset;

  if(parameters.cp_cinema)
  {
    rates = (float*)malloc(parameters.tcp_numlayers * sizeof(float));
    for(int i=0; i< parameters.tcp_numlayers; i++)
    {
      rates[i] = parameters.tcp_rates[i];
    }
    cinema_parameters(&parameters);
  }

  /* Create comment for codestream */
  const char comment[] = "Created by "PACKAGE_STRING;
  parameters.cp_comment = g_strdup(comment);

  /*Converting the image to a format suitable for encoding*/
  {
    int subsampling_dx = parameters.subsampling_dx;
    int subsampling_dy = parameters.subsampling_dy;
    int numcomps = 3;
    int prec = 12; //TODO: allow other bitdepths!
    int w = j2k->width, h = j2k->height;

    opj_image_cmptparm_t cmptparm[4]; /* RGBA: max. 4 components */
    memset(&cmptparm[0], 0, numcomps * sizeof(opj_image_cmptparm_t));

    for(int i = 0; i < numcomps; i++)
    {
      cmptparm[i].prec = prec;
      cmptparm[i].bpp = prec;
      cmptparm[i].sgnd = 0;
      cmptparm[i].dx = subsampling_dx;
      cmptparm[i].dy = subsampling_dy;
      cmptparm[i].w = w;
      cmptparm[i].h = h;
    }
    image = opj_image_create(numcomps, &cmptparm[0], CLRSPC_SRGB);
    if(!image)
    {
      fprintf(stderr, "Error: opj_image_create() failed\n");
      free(rates);
      return 1;
    }

    /* set image offset and reference grid */
    image->x0 = parameters.image_offset_x0;
    image->y0 = parameters.image_offset_y0;
    image->x1 = parameters.image_offset_x0 + (w - 1) * subsampling_dx + 1;
    image->y1 = parameters.image_offset_y0 + (h - 1) * subsampling_dy + 1;

    switch(prec)
    {
      case 8:
        for(int i = 0; i < w * h; i++)
        {
          for(int k = 0; k < numcomps; k++) image->comps[k].data[i] = DOWNSAMPLE_FLOAT_TO_8BIT(in[i*4 + k]);
        }
        break;
      case 12:
        for(int i = 0; i < w * h; i++)
        {
          for(int k = 0; k < numcomps; k++) image->comps[k].data[i] = DOWNSAMPLE_FLOAT_TO_12BIT(in[i*4 + k]);
        }
        break;
      case 16:
        for(int i = 0; i < w * h; i++)
        {
          for(int k = 0; k < numcomps; k++) image->comps[k].data[i] = DOWNSAMPLE_FLOAT_TO_16BIT(in[i*4 + k]);
        }
        break;
      default:
        fprintf(stderr, "Error: this shouldn't happen, there is no bit depth of %d for jpeg 2000 images.\n", prec);
        free(rates);
        return 1;
    }
  }

  /*Encoding image*/

  /* Decide if MCT should be used */
  parameters.tcp_mct = image->numcomps == 3 ? 1 : 0;

  if(parameters.cp_cinema)
  {
    cinema_setup_encoder(&parameters,image,rates);
    free(rates);
  }

  /* encode the destination image */
  /* ---------------------------- */
  int rc = 1;
  OPJ_CODEC_FORMAT codec;
  if(parameters.cod_format == J2K_CFMT)        /* J2K format output */
    codec = CODEC_J2K;
  else
    codec = CODEC_JP2;

  int codestream_length;
  size_t res;
  opj_cio_t *cio = NULL;
  FILE *f = NULL;

  /* get a J2K/JP2 compressor handle */
  opj_cinfo_t* cinfo = opj_create_compress(codec);

  /* catch events using our callbacks and give a local context */
  opj_set_event_mgr((opj_common_ptr)cinfo, &event_mgr, stderr);

  /* setup the encoder parameters using the current image and user parameters */
  opj_setup_encoder(cinfo, &parameters, image);

  /* open a byte stream for writing */
  /* allocate memory for all tiles */
  cio = opj_cio_open((opj_common_ptr)cinfo, NULL, 0);

  /* encode the image */
  if(!opj_encode(cinfo, cio, image, NULL))
  {
    opj_cio_close(cio);
    fprintf(stderr, "failed to encode image\n");
    return 1;
  }
  codestream_length = cio_tell(cio);

  /* write the buffer to disk */
  f = fopen(filename, "wb");
  if(!f)
  {
    fprintf(stderr, "failed to open %s for writing\n", filename);
    return 1;
  }
  res = fwrite(cio->buffer, 1, codestream_length, f);
  if(res < (size_t)codestream_length) /* FIXME */
  {
    fprintf(stderr, "failed to write %d (%s)\n", codestream_length, filename);
    fclose(f);
    return 1;
  }
  fclose(f);

  /* close and free the byte stream */
  opj_cio_close(cio);

  /* free remaining compression structures */
  opj_destroy_compress(cinfo);

  /* add exif data blob. seems to not work for j2k files :( */
  if(exif && j2k->format == JP2_CFMT)
    rc = dt_exif_write_blob(exif,exif_len,filename);

  /* free image data */
  opj_image_destroy(image);

  /* free user parameters structure */
  g_free(parameters.cp_comment);
  if(parameters.cp_matrice) free(parameters.cp_matrice);

  return ((rc == 1) ? 0 : 1);
}

size_t
params_size(dt_imageio_module_format_t *self)
{
  return sizeof(dt_imageio_j2k_t);
}

void*
get_params(dt_imageio_module_format_t *self)
{
  dt_imageio_j2k_t *d = (dt_imageio_j2k_t *)malloc(sizeof(dt_imageio_j2k_t));
  memset(d, 0, sizeof(dt_imageio_j2k_t));
  d->bpp = 16; // can be 8, 12 or 16
  d->format = dt_conf_get_int("plugins/imageio/format/j2k/format");
  d->preset = dt_conf_get_int("plugins/imageio/format/j2k/preset");
  d->quality = dt_conf_get_int("plugins/imageio/format/j2k/quality");
  if(d->quality <= 0 || d->quality > 100) d->quality = 100;
  return d;
}

void
free_params(dt_imageio_module_format_t *self, dt_imageio_module_data_t *params)
{
  free(params);
}

int
set_params(dt_imageio_module_format_t *self, const void *params, const int size)
{
  if(size != self->params_size(self)) return 1;
  dt_imageio_j2k_t *d = (dt_imageio_j2k_t *)params;
  dt_imageio_j2k_gui_t *g = (dt_imageio_j2k_gui_t *)self->gui_data;
  if(d->format == JP2_CFMT) gtk_toggle_button_set_active(g->jp2, TRUE);
  else                      gtk_toggle_button_set_active(g->j2k, TRUE);
  gtk_combo_box_set_active(g->preset, d->preset);
  dtgtk_slider_set_value(g->quality, d->quality);
  return 0;
}

int bpp(dt_imageio_module_data_t *p)
{
  return 32;
}

int levels(dt_imageio_module_data_t *p)
{
  // TODO: adapt as soon as this module supports various bitdepths
  return IMAGEIO_RGB|IMAGEIO_INT12;
}

const char*
mime(dt_imageio_module_data_t *data)
{
  return "image/jp2";
}

const char*
extension(dt_imageio_module_data_t *data_tmp)
{
  dt_imageio_j2k_t*data=(dt_imageio_j2k_t*)data_tmp;
  if(data->format == J2K_CFMT)
    return "j2k";
  else
    return "jp2";
}

const char*
name ()
{
  return _("JPEG 2000");
}

static void combobox_changed(GtkComboBox *widget, gpointer user_data)
{
  int preset = gtk_combo_box_get_active(widget);
  dt_conf_set_int("plugins/imageio/format/j2k/preset", preset);
}

static void radiobutton_changed(GtkRadioButton *radiobutton, gpointer user_data)
{
  int format = GPOINTER_TO_INT(user_data);
  if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(radiobutton)))
    dt_conf_set_int("plugins/imageio/format/j2k/format", format);
}

static void quality_changed(GtkDarktableSlider *slider, gpointer user_data)
{
  int quality = (int)dtgtk_slider_get_value(slider);
  dt_conf_set_int("plugins/imageio/format/j2k/quality", quality);
}

// TODO: some quality/compression stuff in case "off" is selected
void gui_init(dt_imageio_module_format_t *self)
{
  dt_imageio_j2k_gui_t *gui = (dt_imageio_j2k_gui_t *)malloc(sizeof(dt_imageio_j2k_gui_t));
  self->gui_data = (void *)gui;
  self->widget = gtk_vbox_new(TRUE, 5);
  GtkWidget *hbox = gtk_hbox_new(TRUE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);

  int format_last  = dt_conf_get_int("plugins/imageio/format/j2k/format");
  int preset_last  = dt_conf_get_int("plugins/imageio/format/j2k/preset");
  int quality_last = dt_conf_get_int("plugins/imageio/format/j2k/quality");

  GtkWidget *radiobutton = gtk_radio_button_new_with_label(NULL, _("jp2"));
  gui->jp2 = GTK_TOGGLE_BUTTON(radiobutton);
  gtk_box_pack_start(GTK_BOX(hbox), radiobutton, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(radiobutton), "toggled", G_CALLBACK(radiobutton_changed), GINT_TO_POINTER(JP2_CFMT));
  if(format_last == JP2_CFMT) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radiobutton), TRUE);
  radiobutton = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(radiobutton), _("J2K"));
  gui->j2k = GTK_TOGGLE_BUTTON(radiobutton);
  gtk_box_pack_start(GTK_BOX(hbox), radiobutton, TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(radiobutton), "toggled", G_CALLBACK(radiobutton_changed), GINT_TO_POINTER(J2K_CFMT));
  if(format_last == J2K_CFMT) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(radiobutton), TRUE);

  gui->quality = DTGTK_SLIDER(dtgtk_slider_new_with_range(DARKTABLE_SLIDER_BAR, 1, 100, 1, 97, 0));
  dtgtk_slider_set_label(gui->quality,_("quality"));
  dtgtk_slider_set_default_value(gui->quality, 97);
  if(quality_last > 0 && quality_last <= 100)
    dtgtk_slider_set_value(gui->quality, quality_last);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(gui->quality), TRUE, TRUE, 0);
  g_signal_connect (G_OBJECT (gui->quality), "value-changed", G_CALLBACK (quality_changed), NULL);

  hbox = gtk_hbox_new(FALSE, 5);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);
  GtkWidget *label = gtk_label_new(_("DCP mode"));
  gtk_misc_set_alignment(GTK_MISC(label), 0.0, 0.5);
  gtk_box_pack_start(GTK_BOX(hbox), label, TRUE, TRUE, 0);
  GtkComboBoxText *combo = GTK_COMBO_BOX_TEXT(gtk_combo_box_text_new());
  gui->preset = GTK_COMBO_BOX(combo);
  gtk_combo_box_text_append_text(combo, _("off"));
  gtk_combo_box_text_append_text(combo, _("Cinema2K, 24FPS"));
  gtk_combo_box_text_append_text(combo, _("Cinema2K, 48FPS"));
  gtk_combo_box_text_append_text(combo, _("Cinema4K, 24FPS"));
  gtk_combo_box_set_active(GTK_COMBO_BOX(combo), preset_last);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(combo), TRUE, TRUE, 0);
  g_signal_connect(G_OBJECT(combo), "changed", G_CALLBACK(combobox_changed), NULL);

  // TODO: options for "off"
}

void gui_cleanup (dt_imageio_module_format_t *self)
{
  free(self->gui_data);
}

void gui_reset(dt_imageio_module_format_t *self) {}

int flags(dt_imageio_module_data_t *data)
{
  dt_imageio_j2k_t *j = (dt_imageio_j2k_t*)data;
  return (j->format == JP2_CFMT?FORMAT_FLAGS_SUPPORT_XMP:0);
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
