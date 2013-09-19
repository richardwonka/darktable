/*
    This file is part of darktable,
    copyright (c) 2009--2013 johannes hanika.

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
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include <xmmintrin.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include "common/darktable.h"
#include "develop/develop.h"
#include "develop/tiling.h"
#include "control/control.h"
#include "common/colorspaces.h"
#include "common/opencl.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "libraw/libraw.h"
#include "iop/wb_presets.c"
#include "bauhaus/bauhaus.h"

DT_MODULE(2)

#define DT_IOP_LOWEST_TEMPERATURE     2000
#define DT_IOP_HIGHEST_TEMPERATURE   23000

typedef struct dt_iop_temperature_params_t
{
  float temp_out;
  float coeffs[3];
}
dt_iop_temperature_params_t;

typedef struct dt_iop_temperature_gui_data_t
{
  GtkWidget *scale_k, *scale_tint, *scale_r, *scale_g, *scale_b;
  GtkWidget *presets;
  GtkWidget *finetune;
  int preset_cnt;
  int preset_num[50];
  float daylight_wb[3];
}
dt_iop_temperature_gui_data_t;

typedef struct dt_iop_temperature_data_t
{
  float coeffs[3];
}
dt_iop_temperature_data_t;


typedef struct dt_iop_temperature_global_data_t
{
  int kernel_whitebalance_1ui;
  int kernel_whitebalance_4f;
  int kernel_whitebalance_1f;
}
dt_iop_temperature_global_data_t;

const char *name()
{
  return C_("modulename", "white balance");
}


int
groups ()
{
  return IOP_GROUP_BASIC;
}

int
flags ()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}

void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "tint"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "temperature"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "red"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "green"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "blue"));
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_temperature_gui_data_t *g =
    (dt_iop_temperature_gui_data_t*)self->gui_data;

  dt_accel_connect_slider_iop(self, "tint", GTK_WIDGET(g->scale_tint));
  dt_accel_connect_slider_iop(self, "temperature", GTK_WIDGET(g->scale_k));
  dt_accel_connect_slider_iop(self, "red", GTK_WIDGET(g->scale_r));
  dt_accel_connect_slider_iop(self, "green", GTK_WIDGET(g->scale_g));
  dt_accel_connect_slider_iop(self, "blue", GTK_WIDGET(g->scale_b));
}


int
output_bpp(dt_iop_module_t *module, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  if(!dt_dev_pixelpipe_uses_downsampled_input(pipe) && (pipe->image.flags & DT_IMAGE_RAW)) return sizeof(float);
  else return 4*sizeof(float);
}


// ufraw for teh win:
static void
convert_k_to_rgb (float T, float *rgb)
{
  if (T < DT_IOP_LOWEST_TEMPERATURE)  T = DT_IOP_LOWEST_TEMPERATURE;
  if (T > DT_IOP_HIGHEST_TEMPERATURE) T = DT_IOP_HIGHEST_TEMPERATURE;

  /* Convert between Temperature and RGB.
   * Base on information from http://www.brucelindbloom.com/
   * The fit for D-illuminant between 4000K and 23000K are from CIE
   * The generalization to 2000K < T < 4000K and the blackbody fits
   * are my own and should be taken with a grain of salt.
   */
  const double XYZ_to_RGB[3][3] =
  {
    { 3.24071,	-0.969258,  0.0556352 },
    { -1.53726,	1.87599,    -0.203996 },
    { -0.498571,	0.0415557,  1.05707 }
  };

  int c;
  double xD, yD, X, Y, Z, max;
  // Fit for CIE Daylight illuminant
  if (T <= 4000)
  {
    xD = 0.27475e9 / (T * T * T) - 0.98598e6 / (T * T) + 1.17444e3 / T + 0.145986;
  }
  else if (T <= 7000)
  {
    xD = -4.6070e9 / (T * T * T) + 2.9678e6 / (T * T) + 0.09911e3 / T + 0.244063;
  }
  else
  {
    xD = -2.0064e9 / (T * T * T) + 1.9018e6 / (T * T) + 0.24748e3 / T + 0.237040;
  }
  yD = -3 * xD * xD + 2.87 * xD - 0.275;

  // Fit for Blackbody using CIE standard observer function at 2 degrees
  //xD = -1.8596e9/(T*T*T) + 1.37686e6/(T*T) + 0.360496e3/T + 0.232632;
  //yD = -2.6046*xD*xD + 2.6106*xD - 0.239156;

  // Fit for Blackbody using CIE standard observer function at 10 degrees
  //xD = -1.98883e9/(T*T*T) + 1.45155e6/(T*T) + 0.364774e3/T + 0.231136;
  //yD = -2.35563*xD*xD + 2.39688*xD - 0.196035;

  X = xD / yD;
  Y = 1;
  Z = (1 - xD - yD) / yD;
  max = 0;
  for (c = 0; c < 3; c++)
  {
    rgb[c] = X * XYZ_to_RGB[0][c] + Y * XYZ_to_RGB[1][c] + Z * XYZ_to_RGB[2][c];
    if (rgb[c] > max) max = rgb[c];
  }
  for (c = 0; c < 3; c++) rgb[c] = rgb[c] / max;
}

// binary search inversion inspired by ufraw's RGB_to_Temperature:
static void
convert_rgb_to_k(float rgb[3], float *temp, float *tint)
{
  float tmin, tmax, tmp[3];
  for(int k=0; k<3; k++) tmp[k] = rgb[k];
  tmin = DT_IOP_LOWEST_TEMPERATURE;
  tmax = DT_IOP_HIGHEST_TEMPERATURE;
  for (*temp=(tmax+tmin)/2; tmax-tmin>1; *temp=(tmax+tmin)/2)
  {
    convert_k_to_rgb(*temp, tmp);
    if (tmp[2]/tmp[0] > rgb[2]/rgb[0])
      tmax = *temp;
    else
      tmin = *temp;
  }
  *tint = (tmp[1]/tmp[0])/(rgb[1]/rgb[0]);
  if(*tint < 0.2f) *tint = 0.2f;
  if(*tint > 2.5f) *tint = 2.5f;
}

static int
FC(const int row, const int col, const unsigned int filters)
{
  return filters >> (((row << 1 & 14) + (col & 1)) << 1) & 3;
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const int filters = dt_image_flipped_filter(&piece->pipe->image);
  dt_iop_temperature_data_t *d = (dt_iop_temperature_data_t *)piece->data;
  if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && filters && piece->pipe->image.bpp != 4)
  {
    const float coeffsi[3] = {d->coeffs[0]/65535.0f, d->coeffs[1]/65535.0f, d->coeffs[2]/65535.0f};
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid, d) schedule(static)
#endif
    for(int j=0; j<roi_out->height; j++)
    {
      int i=0;
      const uint16_t *in = ((uint16_t *)ivoid) + j*roi_out->width;
      float *out = ((float*)ovoid) + j*roi_out->width;

      // process unaligned pixels
      for ( ; i < ((4-(j*roi_out->width & 3)) & 3) ; i++,out++,in++)
        *out = *in * coeffsi[FC(j+roi_out->y, i+roi_out->x, filters)];

      const __m128 coeffs = _mm_set_ps(coeffsi[FC(j+roi_out->y, roi_out->x+i+3, filters)],
                                       coeffsi[FC(j+roi_out->y, roi_out->x+i+2, filters)],
                                       coeffsi[FC(j+roi_out->y, roi_out->x+i+1, filters)],
                                       coeffsi[FC(j+roi_out->y, roi_out->x+i  , filters)]);

      // process aligned pixels with SSE
      for( ; i < roi_out->width - 3 ; i+=4,out+=4,in+=4)
      {
        _mm_stream_ps(out,_mm_mul_ps(coeffs,_mm_set_ps(in[3],in[2],in[1],in[0])));
      }

      // process the rest
      for( ; i<roi_out->width; i++,out++,in++)
        *out = *in * coeffsi[FC(j+roi_out->y, i+roi_out->x, filters)];
    }
    _mm_sfence();
  }
  else if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && filters && piece->pipe->image.bpp == 4)
  {
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid, d) schedule(static)
#endif
    for(int j=0; j<roi_out->height; j++)
    {
      const float *in = ((float *)ivoid) + j*roi_out->width;
      float *out = ((float*)ovoid) + j*roi_out->width;
      for(int i=0; i<roi_out->width; i++,out++,in++)
        *out = *in * d->coeffs[FC(j+roi_out->x, i+roi_out->y, filters)];
    }
  }
  else
  {
    const int ch = piece->colors;
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, ivoid, ovoid, d) schedule(static)
#endif
    for(int k=0; k<roi_out->height; k++)
    {
      const float *in = ((float*)ivoid) + ch*k*roi_out->width;
      float *out = ((float*)ovoid) + ch*k*roi_out->width;
      for (int j=0; j<roi_out->width; j++,in+=ch,out+=ch)
        for(int c=0; c<3; c++) out[c] = in[c]*d->coeffs[c];
    }
  }
  for(int k=0; k<3; k++)
    piece->pipe->processed_maximum[k] = d->coeffs[k] * piece->pipe->processed_maximum[k];
}

#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_temperature_data_t *d = (dt_iop_temperature_data_t *)piece->data;
  dt_iop_temperature_global_data_t *gd = (dt_iop_temperature_global_data_t *)self->data;

  const int devid = piece->pipe->devid;
  const int filters = dt_image_flipped_filter(&piece->pipe->image);
  float coeffs[3] = {d->coeffs[0], d->coeffs[1], d->coeffs[2]};
  cl_mem dev_coeffs = NULL;
  cl_int err = -999;
  int kernel = -1;

  if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && filters && piece->pipe->image.bpp != 4)
  {
    kernel = gd->kernel_whitebalance_1ui;
    for(int k=0; k<3; k++) coeffs[k] /= 65535.0f;
  }
  else if(!dt_dev_pixelpipe_uses_downsampled_input(piece->pipe) && filters && piece->pipe->image.bpp == 4)
  {
    kernel = gd->kernel_whitebalance_1f;
  }
  else
  {
    kernel = gd->kernel_whitebalance_4f;
  }

  dev_coeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float)*3, coeffs);
  if (dev_coeffs == NULL) goto error;

  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1};
  dt_opencl_set_kernel_arg(devid, kernel, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, kernel, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, kernel, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, kernel, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, kernel, 4, sizeof(cl_mem), (void *)&dev_coeffs);
  dt_opencl_set_kernel_arg(devid, kernel, 5, sizeof(uint32_t), (void *)&filters);
  dt_opencl_set_kernel_arg(devid, kernel, 6, sizeof(uint32_t), (void *)&roi_out->x);
  dt_opencl_set_kernel_arg(devid, kernel, 7, sizeof(uint32_t), (void *)&roi_out->y);
  err = dt_opencl_enqueue_kernel_2d(devid, kernel, sizes);
  if(err != CL_SUCCESS) goto error;

  dt_opencl_release_mem_object(dev_coeffs);
  for(int k=0; k<3; k++)
    piece->pipe->processed_maximum[k] = d->coeffs[k] * piece->pipe->processed_maximum[k];
  return TRUE;

error:
  if (dev_coeffs != NULL) dt_opencl_release_mem_object(dev_coeffs);
  dt_print(DT_DEBUG_OPENCL, "[opencl_white_balance] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void tiling_callback  (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, struct dt_develop_tiling_t *tiling)
{
  tiling->factor = 2.0f; // in + out
  tiling->maxbuf = 1.0f;
  tiling->overhead = 0;
  tiling->overlap = 0;
  tiling->xalign = 2; // Bayer pattern
  tiling->yalign = 2; // Bayer pattern
  return;
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)p1;
  dt_iop_temperature_data_t *d = (dt_iop_temperature_data_t *)piece->data;
  for(int k=0; k<3; k++) d->coeffs[k]  = p->coeffs[k];
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_temperature_data_t));
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  free(piece->data);
  piece->data = NULL;
}

void gui_update (struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  self->request_color_pick = 0;
  self->color_picker_box[0] = self->color_picker_box[1] = .25f;
  self->color_picker_box[2] = self->color_picker_box[3] = .75f;
  self->color_picker_point[0] = self->color_picker_point[1] = 0.5f;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p  = (dt_iop_temperature_params_t *)module->params;
  dt_iop_temperature_params_t *fp = (dt_iop_temperature_params_t *)module->default_params;
  float temp, tint, mul[3];
  for(int k=0; k<3; k++) mul[k] = g->daylight_wb[k]/p->coeffs[k];
  convert_rgb_to_k(mul, &temp, &tint);

  dt_bauhaus_slider_set(g->scale_r, p->coeffs[0]);
  dt_bauhaus_slider_set(g->scale_g, p->coeffs[1]);
  dt_bauhaus_slider_set(g->scale_b, p->coeffs[2]);
  dt_bauhaus_slider_set(g->scale_k, temp);
  dt_bauhaus_slider_set(g->scale_tint, tint);

  dt_bauhaus_combobox_clear(g->presets);
  dt_bauhaus_combobox_add(g->presets, _("camera white balance"));
  dt_bauhaus_combobox_add(g->presets, _("spot white balance"));
  dt_bauhaus_combobox_add(g->presets, _("passthrough"));
  g->preset_cnt = 3;
  const char *wb_name = NULL;
  char makermodel[1024];
  char *model = makermodel;
  dt_colorspaces_get_makermodel_split(makermodel, 1024, &model, self->dev->image_storage.exif_maker, self->dev->image_storage.exif_model);
  if(!dt_image_is_ldr(&self->dev->image_storage)) for(int i=0; i<wb_preset_count; i++)
    {
      if(g->preset_cnt >= 50) break;
      if(!strcmp(wb_preset[i].make,  makermodel) &&
          !strcmp(wb_preset[i].model, model))
      {
        if(!wb_name || strcmp(wb_name, wb_preset[i].name))
        {
          wb_name = wb_preset[i].name;
          dt_bauhaus_combobox_add(g->presets, _(wb_preset[i].name));
          g->preset_num[g->preset_cnt++] = i;
        }
      }
    }

  if(fabsf(p->coeffs[0]-fp->coeffs[0]) + fabsf(p->coeffs[1]-fp->coeffs[1]) + fabsf(p->coeffs[2]-fp->coeffs[2]) < 0.01)
    dt_bauhaus_combobox_set(g->presets, 0);
  else
    dt_bauhaus_combobox_set(g->presets, -1);
  dt_bauhaus_slider_set(g->finetune, 0);
  gtk_widget_set_sensitive(g->finetune, 0);
}

void reload_defaults(dt_iop_module_t *module)
{
  // raw images need wb (to convert from uint16_t to float):
  if(dt_image_is_raw(&module->dev->image_storage))
  {
    module->default_enabled = 1;
    module->hide_enable_button = 1;
  }
  else module->default_enabled = 0;
  dt_iop_temperature_params_t tmp = (dt_iop_temperature_params_t)
  {
    5000.0, {1.0, 1.0, 1.0}
  };

  // get white balance coefficients, as shot
  char filename[DT_MAX_PATH_LEN];
  int ret=0;
  /* check if file is raw / hdr */
  if(dt_image_is_raw(&module->dev->image_storage))
  {
    gboolean from_cache = TRUE;
    dt_image_full_path(module->dev->image_storage.id, filename, DT_MAX_PATH_LEN, &from_cache);
    libraw_data_t *raw = libraw_init(0);

    ret = libraw_open_file(raw, filename);
    if(!ret)
    {
      module->default_enabled = 1;

      for(int k=0; k<3; k++) tmp.coeffs[k] = raw->color.cam_mul[k];
      if(tmp.coeffs[0] <= 0.0)
      {
        for(int k=0; k<3; k++) tmp.coeffs[k] = raw->color.pre_mul[k];
      }
      if(tmp.coeffs[0] == 0 || tmp.coeffs[1] == 0 || tmp.coeffs[2] == 0)
      {
        // could not get useful info, try presets:
        char makermodel[1024];
        char *model = makermodel;
        dt_colorspaces_get_makermodel_split(makermodel, 1024, &model,
                                            module->dev->image_storage.exif_maker,
                                            module->dev->image_storage.exif_model);
        for(int i=0; i<wb_preset_count; i++)
        {
          if(!strcmp(wb_preset[i].make,  makermodel) &&
              !strcmp(wb_preset[i].model, model))
          {
            // just take the first preset we find for this camera
            for(int k=0; k<3; k++) tmp.coeffs[k] = wb_preset[i].channel[k];
            break;
          }
        }
        if(tmp.coeffs[0] == 0 || tmp.coeffs[1] == 0 || tmp.coeffs[2] == 0)
        {
          // final security net: hardcoded default that fits most cams.
          tmp.coeffs[0] = 2.0f;
          tmp.coeffs[1] = 1.0f;
          tmp.coeffs[2] = 1.5f;
        }
      }
      else
      {
        tmp.coeffs[0] /= tmp.coeffs[1];
        tmp.coeffs[2] /= tmp.coeffs[1];
        tmp.coeffs[1] = 1.0f;
      }
      // remember daylight wb used for temperature/tint conversion,
      // assuming it corresponds to CIE daylight
      if(module->gui_data)
      {
        dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)module->gui_data;
        for(int c = 0; c < 3; c++)
          g->daylight_wb[c] = raw->color.pre_mul[c];
        float temp, tint, mul[3];
        for(int k=0; k<3; k++) mul[k] = g->daylight_wb[k]/tmp.coeffs[k];
        convert_rgb_to_k(mul, &temp, &tint);
        dt_bauhaus_slider_set_default(g->scale_k,    temp);
        dt_bauhaus_slider_set_default(g->scale_tint, tint);
      }

    }
    libraw_close(raw);
  }

  memcpy(module->params, &tmp, sizeof(dt_iop_temperature_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_temperature_params_t));
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_temperature_global_data_t *gd = (dt_iop_temperature_global_data_t *)malloc(sizeof(dt_iop_temperature_global_data_t));
  module->data = gd;
  gd->kernel_whitebalance_1ui = dt_opencl_create_kernel(program, "whitebalance_1ui");
  gd->kernel_whitebalance_4f  = dt_opencl_create_kernel(program, "whitebalance_4f");
  gd->kernel_whitebalance_1f  = dt_opencl_create_kernel(program, "whitebalance_1f");
}

void init (dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_temperature_params_t));
  module->default_params = malloc(sizeof(dt_iop_temperature_params_t));
  module->priority = 35; // module order created by iop_dependencies.py, do not edit!
  module->params_size = sizeof(dt_iop_temperature_params_t);
  module->gui_data = NULL;
}

void cleanup (dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_temperature_global_data_t *gd = (dt_iop_temperature_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_whitebalance_1ui);
  dt_opencl_free_kernel(gd->kernel_whitebalance_4f);
  dt_opencl_free_kernel(gd->kernel_whitebalance_1f);
  free(module->data);
  module->data = NULL;
}

static void
gui_update_from_coeffs (dt_iop_module_t *self)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p  = (dt_iop_temperature_params_t *)self->params;
  // now get temp/tint from rgb.
  float temp, tint, mul[3];

  for(int k=0; k<3; k++) mul[k] = g->daylight_wb[k]/p->coeffs[k];
  convert_rgb_to_k(mul, &temp, &tint);

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->scale_k,    temp);
  dt_bauhaus_slider_set(g->scale_tint, tint);
  dt_bauhaus_slider_set(g->scale_r, p->coeffs[0]);
  dt_bauhaus_slider_set(g->scale_g, p->coeffs[1]);
  dt_bauhaus_slider_set(g->scale_b, p->coeffs[2]);
  darktable.gui->reset = 0;
}


static gboolean
expose (GtkWidget *widget, GdkEventExpose *event, dt_iop_module_t *self)
{
  // capture gui color picked event.
  if(darktable.gui->reset) return FALSE;
  if(self->picked_color_max[0] < self->picked_color_min[0]) return FALSE;
  if(!self->request_color_pick) return FALSE;
  static float old[3] = {0, 0, 0};
  const float *grayrgb = self->picked_color;
  if(grayrgb[0] == old[0] && grayrgb[1] == old[1] && grayrgb[2] == old[2]) return FALSE;
  for(int k=0; k<3; k++) old[k] = grayrgb[k];
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
  for(int k=0; k<3; k++) p->coeffs[k] = (grayrgb[k] > 0.001f) ? 1.0f/grayrgb[k] : 1.0f;
  // normalize green:
  p->coeffs[0] /= p->coeffs[1];
  p->coeffs[2] /= p->coeffs[1];
  p->coeffs[1] = 1.0;
  for(int k=0; k<3; k++) p->coeffs[k] = fmaxf(0.0f, fminf(8.0f, p->coeffs[k]));
  gui_update_from_coeffs(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  return FALSE;
}

static void
temp_changed(dt_iop_module_t *self)
{
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p  = (dt_iop_temperature_params_t *)self->params;

  const float temp = dt_bauhaus_slider_get(g->scale_k);
  const float tint = dt_bauhaus_slider_get(g->scale_tint);

  convert_k_to_rgb(temp, p->coeffs);
  // apply green tint
  p->coeffs[1] /= tint;
  // relative to daylight wb:
  for(int k=0; k<3; k++) p->coeffs[k] = g->daylight_wb[k]/p->coeffs[k];
  // normalize:
  p->coeffs[0] /= p->coeffs[1];
  p->coeffs[2] /= p->coeffs[1];
  p->coeffs[1] = 1.0f;

  darktable.gui->reset = 1;
  dt_bauhaus_slider_set(g->scale_r, p->coeffs[0]);
  dt_bauhaus_slider_set(g->scale_g, p->coeffs[1]);
  dt_bauhaus_slider_set(g->scale_b, p->coeffs[2]);
  darktable.gui->reset = 0;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
tint_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  temp_changed(self);
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_bauhaus_combobox_set(g->presets, -1);
}

static void
temp_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  temp_changed(self);
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_bauhaus_combobox_set(g->presets, -1);
}

static void
rgb_callback (GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t *)self->params;
  const float value = dt_bauhaus_slider_get( slider );
  if     (slider == g->scale_r) p->coeffs[0] = value;
  else if(slider == g->scale_g) p->coeffs[1] = value;
  else if(slider == g->scale_b) p->coeffs[2] = value;

  gui_update_from_coeffs(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
  dt_bauhaus_combobox_set(g->presets, -1);
}

static void
apply_preset(dt_iop_module_t *self)
{
  self->request_color_pick = 0;
  if(self->dt->gui->reset) return;
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p  = (dt_iop_temperature_params_t *)self->params;
  dt_iop_temperature_params_t *fp = (dt_iop_temperature_params_t *)self->default_params;
  const int tune = dt_bauhaus_slider_get(g->finetune);
  const int pos = dt_bauhaus_combobox_get(g->presets);
  switch(pos)
  {
    case -1: // just un-setting.
      return;
    case 0: // camera wb
      for(int k=0; k<3; k++) p->coeffs[k] = fp->coeffs[k];
      break;
    case 1: // spot wb, exposure callback will set p->coeffs.
      for(int k=0; k<3; k++) p->coeffs[k] = fp->coeffs[k];
      dt_iop_request_focus(self);
      self->request_color_pick = 1;

      /* set the area sample size*/
      if (self->request_color_pick)
        dt_lib_colorpicker_set_area(darktable.lib, 0.99);

      break;
    case 2: // passthrough mode, raw data
      for(int k=0; k<3; k++) p->coeffs[k] = 1.0;
      break;
    default:
      for(int i=g->preset_num[pos]; i<wb_preset_count; i++)
      {
        char makermodel[1024];
        char *model = makermodel;
        dt_colorspaces_get_makermodel_split(makermodel, 1024, &model, self->dev->image_storage.exif_maker, self->dev->image_storage.exif_model);
        if(!strcmp(wb_preset[i].make,  makermodel) &&
            !strcmp(wb_preset[i].model, model) && wb_preset[i].tuning == tune)
        {
          for(int k=0; k<3; k++) p->coeffs[k] = wb_preset[i].channel[k];
          break;
        }
      }
      break;
  }
  if(self->off) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(self->off), 1);
  gui_update_from_coeffs(self);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void
presets_changed (GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  apply_preset(self);
  const int pos = dt_bauhaus_combobox_get(widget);
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  gtk_widget_set_sensitive(g->finetune, pos > 2);
}

static void
finetune_changed (GtkWidget *widget, gpointer user_data)
{
  apply_preset((dt_iop_module_t *)user_data);
}

void gui_init (struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_temperature_gui_data_t));
  dt_iop_temperature_gui_data_t *g = (dt_iop_temperature_gui_data_t *)self->gui_data;
  dt_iop_temperature_params_t *p = (dt_iop_temperature_params_t*)self->default_params;

  self->request_color_pick = 0;
  self->widget = gtk_vbox_new(TRUE, DT_BAUHAUS_SPACE);
  g_signal_connect(G_OBJECT(self->widget), "expose-event", G_CALLBACK(expose), self);

  for(int k=0; k<3; k++) g->daylight_wb[k] = 1.0f;
  g->scale_tint  = dt_bauhaus_slider_new_with_range(self,0.1, 8.0, .01,1.0,3);
  g->scale_k     = dt_bauhaus_slider_new_with_range(self,DT_IOP_LOWEST_TEMPERATURE, DT_IOP_HIGHEST_TEMPERATURE, 10.,5000.0,0);
  g->scale_r     = dt_bauhaus_slider_new_with_range(self,0.0, 8.0, .001,p->coeffs[0],3);
  g->scale_g     = dt_bauhaus_slider_new_with_range(self,0.0, 8.0, .001,p->coeffs[1],3);
  g->scale_b     = dt_bauhaus_slider_new_with_range(self,0.0, 8.0, .001,p->coeffs[2],3);
  dt_bauhaus_slider_set_format(g->scale_k,"%.0fK");
  dt_bauhaus_widget_set_label(g->scale_tint, NULL, _("tint"));
  dt_bauhaus_widget_set_label(g->scale_k, NULL, _("temperature"));
  dt_bauhaus_widget_set_label(g->scale_r, NULL, _("red"));
  dt_bauhaus_widget_set_label(g->scale_g, NULL, _("green"));
  dt_bauhaus_widget_set_label(g->scale_b, NULL, _("blue"));

  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_tint, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_k, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_r, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_g, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale_b, TRUE, TRUE, 0);

  g->presets = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->presets, NULL, _("preset"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->presets, TRUE, TRUE, 0);
  g_object_set(G_OBJECT(g->presets), "tooltip-text", _("choose white balance preset from camera"), (char *)NULL);

  g->finetune = dt_bauhaus_slider_new_with_range(self,-9.0, 9.0, 1.0, 0.0, 0);
  dt_bauhaus_widget_set_label(g->finetune, NULL, _("finetune"));
  dt_bauhaus_slider_set_format(g->finetune, _("%.0f mired"));
  // initially doesn't have fine tuning stuff (camera wb)
  gtk_widget_set_sensitive(g->finetune, FALSE);
  gtk_box_pack_start(GTK_BOX(self->widget), g->finetune, TRUE, TRUE, 0);
  g_object_set(G_OBJECT(g->finetune), "tooltip-text", _("fine tune white balance preset"), (char *)NULL);

  self->gui_update(self);

  g_signal_connect (G_OBJECT (g->scale_tint), "value-changed",
                    G_CALLBACK (tint_callback), self);
  g_signal_connect (G_OBJECT (g->scale_k), "value-changed",
                    G_CALLBACK (temp_callback), self);
  g_signal_connect (G_OBJECT (g->scale_r), "value-changed",
                    G_CALLBACK (rgb_callback), self);
  g_signal_connect (G_OBJECT (g->scale_g), "value-changed",
                    G_CALLBACK (rgb_callback), self);
  g_signal_connect (G_OBJECT (g->scale_b), "value-changed",
                    G_CALLBACK (rgb_callback), self);
  g_signal_connect (G_OBJECT (g->presets), "value-changed",
                    G_CALLBACK (presets_changed), self);
  g_signal_connect (G_OBJECT (g->finetune), "value-changed",
                    G_CALLBACK (finetune_changed), self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  self->request_color_pick = 0;
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
