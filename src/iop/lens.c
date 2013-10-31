/*
    This file is part of darktable,
    copyright (c) 2009--2012 johannes hanika.

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
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>
#include <gtk/gtk.h>
#include <inttypes.h>
#include <ctype.h>
#include "develop/develop.h"
#include "develop/imageop.h"
#include "develop/tiling.h"
#include "common/opencl.h"
#include "common/interpolation.h"
#include "control/control.h"
#include "dtgtk/button.h"
#include "dtgtk/resetlabel.h"
#include "bauhaus/bauhaus.h"
#include "gui/accelerators.h"
#include "gui/gtk.h"
#include "gui/draw.h"
#include "iop/lens.h"


DT_MODULE(2)

const char*
name()
{
  return _("lens correction");
}

int
groups ()
{
  return IOP_GROUP_CORRECT;
}

int
operation_tags ()
{
  return IOP_TAG_DISTORT;
}

int flags()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_TILING_FULL_ROI | IOP_FLAGS_ONE_INSTANCE;
}


void init_key_accels(dt_iop_module_so_t *self)
{
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "scale"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "TCA R"));
  dt_accel_register_slider_iop(self, FALSE, NC_("accel", "TCA B"));

  dt_accel_register_iop(self, FALSE, NC_("accel", "find camera"), 0, 0);
  dt_accel_register_iop(self, FALSE, NC_("accel", "find lens"), 0, 0);
  dt_accel_register_iop(self, FALSE, NC_("accel", "auto scale"), 0, 0);
  dt_accel_register_iop(self, FALSE, NC_("accel", "camera model"), 0, 0);
  dt_accel_register_iop(self, FALSE, NC_("accel", "lens model"), 0, 0);
  dt_accel_register_iop(self, FALSE, NC_("accel", "select corrections"), 0, 0);
}

void connect_key_accels(dt_iop_module_t *self)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t*)self->gui_data;

  dt_accel_connect_button_iop(self, "find lens",
                              GTK_WIDGET(g->find_lens_button));
  dt_accel_connect_button_iop(self, "lens model",
                              GTK_WIDGET(g->lens_model));
  dt_accel_connect_button_iop(self, "camera model",
                              GTK_WIDGET(g->camera_model));
  dt_accel_connect_button_iop(self, "find camera",
                              GTK_WIDGET(g->find_camera_button));
  dt_accel_connect_button_iop(self, "select corrections",
                              GTK_WIDGET(g->modflags));

  dt_accel_connect_slider_iop(self, "scale", GTK_WIDGET(g->scale));
  dt_accel_connect_slider_iop(self, "tca R", GTK_WIDGET(g->tca_r));
  dt_accel_connect_slider_iop(self, "tca B", GTK_WIDGET(g->tca_b));
}


static char*
_lens_sanitize(const char *orig_lens)
{
  char *found_or = strstr(orig_lens, " or ");
  char *found_parenthesis = strstr(orig_lens, " (");

  if (found_or || found_parenthesis)
  {
    size_t pos_or = (size_t)(found_or - orig_lens);
    size_t pos_parenthesis = (size_t)(found_parenthesis - orig_lens);
    size_t pos = pos_or < pos_parenthesis ? pos_or : pos_parenthesis;

    if (pos > 0)
    {
      char *new_lens = (char*) malloc(pos+1);

      strncpy(new_lens, orig_lens, pos);
      new_lens[pos] = '\0';

      return new_lens;
    }
    else
    {
      char *new_lens = strdup(orig_lens);
      return new_lens;
    }
  }
  else
  {
    char *new_lens = strdup(orig_lens);
    return new_lens;
  }
}


void
process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *ivoid, void *ovoid, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;

  float *in  = (float *)ivoid;
  float *out = (float *)ovoid;
  const int ch = piece->colors;
  const int ch_width = ch*roi_in->width;
  const int mask_display = piece->pipe->mask_display;

  const unsigned int pixelformat = ch == 3 ? LF_CR_3 (RED, GREEN, BLUE) : LF_CR_4 (RED, GREEN, BLUE, UNKNOWN);

  if(!d->lens->Maker || d->crop <= 0.0f)
  {
    memcpy(out, in, ch*sizeof(float)*roi_out->width*roi_out->height);
    return;
  }

  const float orig_w = roi_in->scale*piece->iwidth,
              orig_h = roi_in->scale*piece->iheight;
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  lfModifier *modifier = lf_modifier_new(d->lens, d->crop, orig_w, orig_h);

  int modflags = lf_modifier_initialize(
                   modifier, d->lens, LF_PF_F32,
                   d->focal, d->aperture,
                   d->distance, d->scale,
                   d->target_geom, d->modify_flags, d->inverse);
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

  if(d->inverse)
  {
    // reverse direction (useful for renderings)
    if (modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION |
                    LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
    {
      // acquire temp memory for distorted pixel coords
      const size_t req2 = roi_out->width*2*3*sizeof(float);
      if(req2 > 0 && d->tmpbuf2_len < req2*dt_get_num_threads())
      {
        d->tmpbuf2_len = req2*dt_get_num_threads();
        dt_free_align(d->tmpbuf2);
        d->tmpbuf2 = (float *)dt_alloc_align(16, d->tmpbuf2_len);
      }

      const struct  dt_interpolation* interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);

#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(roi_out, roi_in, in, d, ovoid, modifier, interpolation) schedule(static)
#endif
      for (int y = 0; y < roi_out->height; y++)
      {
        float *pi = (float *)(((char *)d->tmpbuf2) + req2*dt_get_thread_num());
        lf_modifier_apply_subpixel_geometry_distortion (
          modifier, roi_out->x, roi_out->y+y, roi_out->width, 1, pi);
        // reverse transform the global coords from lf to our buffer
        float *buf = ((float *)ovoid) + y*roi_out->width*ch;
        for (int x = 0; x < roi_out->width; x++,buf+=ch,pi+=6)
        {
          for(int c=0; c<3; c++)
          {
            const float pi0 = pi[c*2] - roi_in->x;
            const float pi1 = pi[c*2+1] - roi_in->y;
            buf[c] = dt_interpolation_compute_sample(interpolation, in+c, pi0, pi1, roi_in->width, roi_in->height, ch, ch_width);
          }

          if(mask_display)
          {
            // take green channel distortion also for alpha channel
            const float pi0 = pi[2] - roi_in->x;
            const float pi1 = pi[3] - roi_in->y;
            buf[3] = dt_interpolation_compute_sample(interpolation, in+3, pi0, pi1, roi_in->width, roi_in->height, ch, ch_width);
          }
        }
      }
    }
    else
    {
#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(roi_out, out, in) schedule(static)
#endif
      for (int y = 0; y < roi_out->height; y++)
        memcpy(out+ch*y*roi_out->width, in+ch*y*roi_out->width, ch*sizeof(float)*roi_out->width);
    }

    if (modflags & LF_MODIFY_VIGNETTING)
    {
#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(roi_out, out, modifier) schedule(static)
#endif
      for (int y = 0; y < roi_out->height; y++)
      {
        /* Colour correction: vignetting and CCI */
        // actually this way row stride does not matter.
        float *buf = out;
        lf_modifier_apply_color_modification (modifier,
                                              buf + ch*roi_out->width*y, roi_out->x, roi_out->y + y,
                                              roi_out->width, 1, pixelformat, ch*roi_out->width);
      }
    }
  }
  else // correct distortions:
  {
    // acquire temp memory for image buffer
    const size_t req = roi_in->width*roi_in->height*ch*sizeof(float);
    if(req > 0 && d->tmpbuf_len < req)
    {
      d->tmpbuf_len = req;
      dt_free_align(d->tmpbuf);
      d->tmpbuf = (float *)dt_alloc_align(16, d->tmpbuf_len);
    }
    memcpy(d->tmpbuf, in, req);
    if (modflags & LF_MODIFY_VIGNETTING)
    {
#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(roi_in, out, modifier, d) schedule(static)
#endif
      for (int y = 0; y < roi_in->height; y++)
      {
        /* Colour correction: vignetting and CCI */
        // actually this way row stride does not matter.
        float *buf = d->tmpbuf;
        lf_modifier_apply_color_modification (modifier,
                                              buf + ch*roi_in->width*y, roi_in->x, roi_in->y + y,
                                              roi_in->width, 1, pixelformat, ch*roi_in->width);
      }
    }

    const size_t req2 = roi_out->width*2*3*sizeof(float);
    if (modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION |
                    LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
    {
      // acquire temp memory for distorted pixel coords
      if(req2 > 0 && d->tmpbuf2_len < req2*dt_get_num_threads())
      {
        d->tmpbuf2_len = req2*dt_get_num_threads();
        dt_free_align(d->tmpbuf2);
        d->tmpbuf2 = (float *)dt_alloc_align(16, d->tmpbuf2_len);
      }

      const struct dt_interpolation* interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);

#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(roi_in, roi_out, d, ovoid, modifier, interpolation) schedule(static)
#endif
      for (int y = 0; y < roi_out->height; y++)
      {
        float *pi = (float *)(((char *)d->tmpbuf2) + dt_get_thread_num()*req2);
        lf_modifier_apply_subpixel_geometry_distortion (
          modifier, roi_out->x, roi_out->y+y, roi_out->width, 1, pi);
        // reverse transform the global coords from lf to our buffer
        float *out = ((float *)ovoid) + y*roi_out->width*ch;
        for (int x = 0; x < roi_out->width; x++,pi+=6)
        {
          for(int c=0; c<3; c++)
          {
            const float pi0 = pi[c*2] - roi_in->x;
            const float pi1 = pi[c*2+1] - roi_in->y;
            out[c] = dt_interpolation_compute_sample(interpolation, d->tmpbuf+c, pi0, pi1, roi_in->width, roi_in->height, ch, ch_width);
          }

          if(mask_display)
          {
            // take green channel distortion also for alpha channel
            const float pi0 = pi[2] - roi_in->x;
            const float pi1 = pi[3] - roi_in->y;
            out[3] = dt_interpolation_compute_sample(interpolation, d->tmpbuf+3, pi0, pi1, roi_in->width, roi_in->height, ch, ch_width);
          }
          out += ch;
        }
      }
    }
    else
    {
      const size_t len = sizeof(float)*ch*roi_out->width*roi_out->height;
      const float *const input = (d->tmpbuf_len >= len) ? d->tmpbuf : in;
#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(roi_out, out) schedule(static)
#endif
      for (int y = 0; y < roi_out->height; y++)
        memcpy(out+ch*y*roi_out->width, input+ch*y*roi_out->width, ch*sizeof(float)*roi_out->width);
    }
  }
  lf_modifier_destroy(modifier);

  if(g != NULL && self->dev->gui_attached && piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
  {
    g->corrections_done = (modflags & LENSFUN_MODFLAG_MASK);
  }
}


#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)self->data;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;

  cl_mem dev_tmpbuf = NULL;
  cl_mem dev_tmp = NULL;
  cl_int err = -999;

  float *tmpbuf = NULL;
  lfModifier *modifier = NULL;

  const int devid = piece->pipe->devid;
  const int iwidth = roi_in->width;
  const int iheight = roi_in->height;
  const int owidth = roi_out->width;
  const int oheight = roi_out->height;
  const int roi_in_x = roi_in->x;
  const int roi_in_y = roi_in->y;
  const int width = MAX(iwidth, owidth);
  const int height = MAX(iheight, oheight);
  const int ch = piece->colors;
  const int tmpbufwidth = owidth*2*3;
  const int tmpbuflen = d->inverse ? oheight*owidth*2*3*sizeof(float) : MAX(oheight*owidth*2*3, iheight*iwidth*ch)*sizeof(float);
  const unsigned int pixelformat = ch == 3 ? LF_CR_3 (RED, GREEN, BLUE) : LF_CR_4 (RED, GREEN, BLUE, UNKNOWN);

  const float orig_w = roi_in->scale*piece->iwidth,
              orig_h = roi_in->scale*piece->iheight;

  size_t origin[] = { 0, 0, 0};
  size_t iregion[] = { iwidth, iheight, 1};
  size_t oregion[] = { owidth, oheight, 1};
  size_t isizes[] = { ROUNDUPWD(iwidth), ROUNDUPHT(iheight), 1};
  size_t osizes[] = { ROUNDUPWD(owidth), ROUNDUPHT(oheight), 1};

  if(!d->lens->Maker || d->crop <= 0.0f)
  {
    err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_out, origin, origin, oregion);
    if(err != CL_SUCCESS) goto error;
    return TRUE;
  }

  const struct dt_interpolation* interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);

  int ldkernel = -1;

  switch(interpolation->id)
  {
    case DT_INTERPOLATION_BILINEAR:
      ldkernel = gd->kernel_lens_distort_bilinear;
      break;
    case DT_INTERPOLATION_BICUBIC:
      ldkernel = gd->kernel_lens_distort_bicubic;
      break;
    case DT_INTERPOLATION_LANCZOS2:
      ldkernel = gd->kernel_lens_distort_lanczos2;
      break;
    case DT_INTERPOLATION_LANCZOS3:
      ldkernel = gd->kernel_lens_distort_lanczos3;
      break;
    default:
      return FALSE;
  }


  tmpbuf = (float *)dt_alloc_align(16, tmpbuflen);
  if(tmpbuf == NULL) goto error;

  dev_tmp = dt_opencl_alloc_device(devid, width, height, 4*sizeof(float));
  if(dev_tmp == NULL) goto error;

  dev_tmpbuf = dt_opencl_alloc_device_buffer(devid, tmpbuflen);
  if(dev_tmpbuf == NULL) goto error;


  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  modifier = lf_modifier_new(d->lens, d->crop, orig_w, orig_h);

  int modflags = lf_modifier_initialize(
                   modifier, d->lens, LF_PF_F32,
                   d->focal, d->aperture,
                   d->distance, d->scale,
                   d->target_geom, d->modify_flags, d->inverse);
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);

  if(d->inverse)
  {
    // reverse direction (useful for renderings)
    if(modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION |
                   LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
    {
#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(roi_out, roi_in, tmpbuf, d, modifier) schedule(static)
#endif
      for (int y = 0; y < roi_out->height; y++)
      {
        float *pi = tmpbuf + y * tmpbufwidth;
        lf_modifier_apply_subpixel_geometry_distortion (
          modifier, roi_out->x, roi_out->y+y, roi_out->width, 1, pi);
      }

      /* _blocking_ memory transfer: host tmpbuf buffer -> opencl dev_tmpbuf */
      err = dt_opencl_write_buffer_to_device(devid, tmpbuf, dev_tmpbuf, 0, owidth*oheight*2*3*sizeof(float), CL_TRUE);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_arg(devid, ldkernel, 0, sizeof(cl_mem), (void *)&dev_in);
      dt_opencl_set_kernel_arg(devid, ldkernel, 1, sizeof(cl_mem), (void *)&dev_tmp);
      dt_opencl_set_kernel_arg(devid, ldkernel, 2, sizeof(int), (void *)&owidth);
      dt_opencl_set_kernel_arg(devid, ldkernel, 3, sizeof(int), (void *)&oheight);
      dt_opencl_set_kernel_arg(devid, ldkernel, 4, sizeof(int), (void *)&iwidth);
      dt_opencl_set_kernel_arg(devid, ldkernel, 5, sizeof(int), (void *)&iheight);
      dt_opencl_set_kernel_arg(devid, ldkernel, 6, sizeof(int), (void *)&roi_in_x);
      dt_opencl_set_kernel_arg(devid, ldkernel, 7, sizeof(int), (void *)&roi_in_y);
      dt_opencl_set_kernel_arg(devid, ldkernel, 8, sizeof(cl_mem), (void *)&dev_tmpbuf);
      err = dt_opencl_enqueue_kernel_2d(devid, ldkernel, osizes);
      if(err != CL_SUCCESS) goto error;

    }
    else
    {
      err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_tmp, origin, origin, oregion);
      if(err != CL_SUCCESS) goto error;
    }

    if(modflags & LF_MODIFY_VIGNETTING)
    {
#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(roi_out, roi_in, tmpbuf, modifier, d) schedule(static)
#endif
      for (int y = 0; y < roi_out->height; y++)
      {
        /* Colour correction: vignetting and CCI */
        // actually this way row stride does not matter.
        float *buf = tmpbuf + y * ch*roi_out->width;
        for (int k=0; k < ch*roi_out->width; k++) buf[k] = 0.5f;
        lf_modifier_apply_color_modification (modifier, buf, roi_out->x, roi_out->y + y,
                                              roi_out->width, 1, pixelformat, ch*roi_out->width);
      }

      /* _blocking_ memory transfer: host tmpbuf buffer -> opencl dev_tmpbuf */
      err = dt_opencl_write_buffer_to_device(devid, tmpbuf, dev_tmpbuf, 0, ch*roi_out->width*roi_out->height*sizeof(float), CL_TRUE);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 0, sizeof(cl_mem), (void *)&dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 1, sizeof(cl_mem), (void *)&dev_out);
      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 2, sizeof(int), (void *)&owidth);
      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 3, sizeof(int), (void *)&oheight);
      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 4, sizeof(cl_mem), (void *)&dev_tmpbuf);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_lens_vignette, osizes);
      if(err != CL_SUCCESS) goto error;

    }
    else
    {
      err = dt_opencl_enqueue_copy_image(devid, dev_tmp, dev_out, origin, origin, oregion);
      if(err != CL_SUCCESS) goto error;
    }

  }

  else // correct distortions:
  {

    if(modflags & LF_MODIFY_VIGNETTING)
    {
#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(roi_out, roi_in, tmpbuf, modifier, d) schedule(static)
#endif
      for (int y = 0; y < roi_in->height; y++)
      {
        /* Colour correction: vignetting and CCI */
        // actually this way row stride does not matter.
        float *buf = tmpbuf + y * ch*roi_in->width;
        for (int k=0; k < ch*roi_in->width; k++) buf[k] = 0.5f;
        lf_modifier_apply_color_modification (modifier, buf, roi_in->x, roi_in->y + y,
                                              roi_in->width, 1, pixelformat, ch*roi_in->width);
      }

      /* _blocking_ memory transfer: host tmpbuf buffer -> opencl dev_tmpbuf */
      err = dt_opencl_write_buffer_to_device(devid, tmpbuf, dev_tmpbuf, 0, ch*roi_in->width*roi_in->height*sizeof(float), CL_TRUE);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 0, sizeof(cl_mem), (void *)&dev_in);
      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 1, sizeof(cl_mem), (void *)&dev_tmp);
      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 2, sizeof(int), (void *)&iwidth);
      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 3, sizeof(int), (void *)&iheight);
      dt_opencl_set_kernel_arg(devid, gd->kernel_lens_vignette, 4, sizeof(cl_mem), (void *)&dev_tmpbuf);
      err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_lens_vignette, isizes);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      err = dt_opencl_enqueue_copy_image(devid, dev_in, dev_tmp, origin, origin, iregion);
      if(err != CL_SUCCESS) goto error;
    }


    if(modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION |
                   LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
    {
#ifdef _OPENMP
      #pragma omp parallel for default(none) shared(roi_out, roi_in, tmpbuf, d, modifier) schedule(static)
#endif
      for (int y = 0; y < roi_out->height; y++)
      {
        float *pi = tmpbuf + y * tmpbufwidth;
        lf_modifier_apply_subpixel_geometry_distortion (
          modifier, roi_out->x, roi_out->y+y, roi_out->width, 1, pi);
      }

      /* _blocking_ memory transfer: host tmpbuf buffer -> opencl dev_tmpbuf */
      err = dt_opencl_write_buffer_to_device(devid, tmpbuf, dev_tmpbuf, 0, owidth*oheight*2*3*sizeof(float), CL_TRUE);
      if(err != CL_SUCCESS) goto error;

      dt_opencl_set_kernel_arg(devid, ldkernel, 0, sizeof(cl_mem), (void *)&dev_tmp);
      dt_opencl_set_kernel_arg(devid, ldkernel, 1, sizeof(cl_mem), (void *)&dev_out);
      dt_opencl_set_kernel_arg(devid, ldkernel, 2, sizeof(int), (void *)&owidth);
      dt_opencl_set_kernel_arg(devid, ldkernel, 3, sizeof(int), (void *)&oheight);
      dt_opencl_set_kernel_arg(devid, ldkernel, 4, sizeof(int), (void *)&iwidth);
      dt_opencl_set_kernel_arg(devid, ldkernel, 5, sizeof(int), (void *)&iheight);
      dt_opencl_set_kernel_arg(devid, ldkernel, 6, sizeof(int), (void *)&roi_in_x);
      dt_opencl_set_kernel_arg(devid, ldkernel, 7, sizeof(int), (void *)&roi_in_y);
      dt_opencl_set_kernel_arg(devid, ldkernel, 8, sizeof(cl_mem), (void *)&dev_tmpbuf);
      err = dt_opencl_enqueue_kernel_2d(devid, ldkernel, osizes);
      if(err != CL_SUCCESS) goto error;
    }
    else
    {
      err = dt_opencl_enqueue_copy_image(devid, dev_tmp, dev_out, origin, origin, oregion);
      if(err != CL_SUCCESS) goto error;
    }

  }

  if(g != NULL && self->dev->gui_attached && piece->pipe->type == DT_DEV_PIXELPIPE_PREVIEW)
  {
    g->corrections_done = (modflags & LENSFUN_MODFLAG_MASK);
  }

  dt_opencl_release_mem_object(dev_tmpbuf);
  dt_opencl_release_mem_object(dev_tmp);
  if (tmpbuf != NULL) dt_free_align(tmpbuf);
  if (modifier != NULL) lf_modifier_destroy(modifier);
  return TRUE;

error:
  if (dev_tmp != NULL) dt_opencl_release_mem_object(dev_tmp);
  if (dev_tmpbuf != NULL) dt_opencl_release_mem_object(dev_tmpbuf);
  if (tmpbuf != NULL) dt_free_align(tmpbuf);
  if (modifier != NULL) lf_modifier_destroy(modifier);
  dt_print(DT_DEBUG_OPENCL, "[opencl_lens] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

void tiling_callback (struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out, struct dt_develop_tiling_t *tiling)
{
  tiling->factor = 4.5f;  // in + out + tmp + tmpbuf
  tiling->maxbuf = 1.5f;
  tiling->overhead = 0;
  tiling->overlap = 4;
  tiling->xalign = 1;
  tiling->yalign = 1;
  return;
}

int distort_transform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points, int points_count)
{
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;

  if(!d->lens->Maker || d->crop <= 0.0f) return 0;

  const float orig_w = piece->iwidth, orig_h = piece->iheight;
  lfModifier *modifier = lf_modifier_new(d->lens, d->crop, orig_w, orig_h);

  int modflags = lf_modifier_initialize(
                   modifier, d->lens, LF_PF_F32,
                   d->focal, d->aperture,
                   d->distance, d->scale,
                   d->target_geom, d->modify_flags, !d->inverse);
  float *buf = malloc(2*3*sizeof(float));

  for (int i=0; i<points_count*2; i+=2)
  {
    if (modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
    {
      lf_modifier_apply_subpixel_geometry_distortion (modifier, points[i], points[i+1], 1, 1, buf);
      points[i] = buf[0];
      points[i+1] = buf[3];
    }
  }
  free(buf);
  lf_modifier_destroy(modifier);

  return 1;
}
int distort_backtransform(dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, float *points, int points_count)
{
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;
  if(!d->lens->Maker || d->crop <= 0.0f) return 0;

  const float orig_w = piece->iwidth, orig_h = piece->iheight;
  lfModifier *modifier = lf_modifier_new(d->lens, d->crop, orig_w, orig_h);

  int modflags = lf_modifier_initialize(
                   modifier, d->lens, LF_PF_F32,
                   d->focal, d->aperture,
                   d->distance, d->scale,
                   d->target_geom, d->modify_flags, d->inverse);
  float *buf = malloc(2*3*sizeof(float));

  for (int i=0; i<points_count*2; i+=2)
  {
    if (modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
    {
      lf_modifier_apply_subpixel_geometry_distortion (modifier, points[i], points[i+1], 1, 1, buf);
      points[i] = buf[0];
      points[i+1] = buf[3];
    }
  }
  free(buf);
  lf_modifier_destroy(modifier);
  return 1;
}

void modify_roi_out(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, dt_iop_roi_t *roi_out, const dt_iop_roi_t *roi_in)
{
  *roi_out = *roi_in;
}

void modify_roi_in(struct dt_iop_module_t *self, struct dt_dev_pixelpipe_iop_t *piece, const dt_iop_roi_t *roi_out, dt_iop_roi_t *roi_in)
{
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;
  *roi_in = *roi_out;
  // inverse transform with given params

  if(!d->lens->Maker || d->crop <= 0.0f) return;

  const float orig_w = roi_in->scale*piece->iwidth,
              orig_h = roi_in->scale*piece->iheight;
  lfModifier *modifier = lf_modifier_new(d->lens, d->crop, orig_w, orig_h);

  float xm = INFINITY, xM = - INFINITY, ym = INFINITY, yM = - INFINITY;

  int modflags = lf_modifier_initialize(
                   modifier, d->lens, LF_PF_F32,
                   d->focal, d->aperture,
                   d->distance, d->scale,
                   d->target_geom, d->modify_flags, d->inverse);

  if (modflags & (LF_MODIFY_TCA | LF_MODIFY_DISTORTION |
                  LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE))
  {
    // acquire temp memory for distorted pixel coords
    const size_t req2 = roi_in->width*2*3*sizeof(float);
    if(req2 > 0 && d->tmpbuf2_len < req2)
    {
      d->tmpbuf2_len = req2;
      dt_free_align(d->tmpbuf2);
      d->tmpbuf2 = (float *)dt_alloc_align(16, d->tmpbuf2_len);
    }
    for (int y = 0; y < roi_out->height; y++)
    {
      lf_modifier_apply_subpixel_geometry_distortion (
        modifier, roi_out->x, roi_out->y+y, roi_out->width, 1, d->tmpbuf2);
      const float *pi = d->tmpbuf2;
      // reverse transform the global coords from lf to our buffer
      for (int x = 0; x < roi_out->width; x++)
      {
        for(int c=0; c<3; c++)
        {
          xm = fminf(xm, pi[0]);
          xM = fmaxf(xM, pi[0]);
          ym = fminf(ym, pi[1]);
          yM = fmaxf(yM, pi[1]);
          pi+=2;
        }
      }
    }

    const struct dt_interpolation* interpolation = dt_interpolation_new(DT_INTERPOLATION_USERPREF);
    roi_in->x = fmaxf(0.0f, xm-interpolation->width);
    roi_in->y = fmaxf(0.0f, ym-interpolation->width);
    roi_in->width = fminf(orig_w-roi_in->x, xM - roi_in->x + interpolation->width);
    roi_in->height = fminf(orig_h-roi_in->y, yM - roi_in->y + interpolation->width);
  }
  lf_modifier_destroy(modifier);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)p1;
#ifdef HAVE_GEGL
  // pull in new params to gegl
#error "lensfun needs to be ported to GEGL!"
#else
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;

  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)self->data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  const lfCamera *camera = NULL;
  const lfCamera **cam = NULL;

  lf_lens_destroy(d->lens);
  d->lens = lf_lens_new();

  if(p->camera[0])
  {
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    cam = lf_db_find_cameras_ext(dt_iop_lensfun_db,
                                 NULL, p->camera, 0);
    if(cam)
    {
      camera = cam[0];
      p->crop = cam[0]->CropFactor;
    }
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }
  if(p->lens[0])
  {
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfLens **lens = lf_db_find_lenses_hd(dt_iop_lensfun_db, camera, NULL,
                          p->lens, 0);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if(lens)
    {
      lf_lens_copy(d->lens, lens[0]);
      if(p->tca_override)
      {
        // add manual d->lens stuff:
        lfLensCalibTCA tca;
        memset(&tca, 0, sizeof(lfLensCalibTCA));
        tca.Focal = 0;
        tca.Model = LF_TCA_MODEL_LINEAR;
        tca.Terms[0] = p->tca_b;
        tca.Terms[1] = p->tca_r;
        if(d->lens->CalibTCA)
          while (d->lens->CalibTCA[0])
            lf_lens_remove_calib_tca (d->lens, 0);
        lf_lens_add_calib_tca (d->lens, &tca);
      }
      lf_free (lens);
    }
  }
  lf_free(cam);
  d->modify_flags = p->modify_flags;
  d->inverse      = p->inverse;
  d->scale        = p->scale;
  d->crop         = p->crop;
  d->focal        = p->focal;
  d->aperture     = p->aperture;
  d->distance     = p->distance;
  d->target_geom  = p->target_geom;
#endif
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
#error "lensfun needs to be ported to GEGL!"
#else
  piece->data = malloc(sizeof(dt_iop_lensfun_data_t));
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;

  d->tmpbuf2_len = 0;
  d->tmpbuf2 = NULL;
  d->tmpbuf_len = 0;
  d->tmpbuf = NULL;
  d->lens = lf_lens_new();
  self->commit_params(self, self->default_params, pipe, piece);
#endif
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
#ifdef HAVE_GEGL
#error "lensfun needs to be ported to GEGL!"
#else
  dt_iop_lensfun_data_t *d = (dt_iop_lensfun_data_t *)piece->data;
  lf_lens_destroy(d->lens);
  dt_free_align(d->tmpbuf);
  dt_free_align(d->tmpbuf2);
  free(piece->data);
#endif
}

void init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)malloc(sizeof(dt_iop_lensfun_global_data_t));
  module->data = gd;
  gd->kernel_lens_distort_bilinear = dt_opencl_create_kernel(program, "lens_distort_bilinear");
  gd->kernel_lens_distort_bicubic = dt_opencl_create_kernel(program, "lens_distort_bicubic");
  gd->kernel_lens_distort_lanczos2 = dt_opencl_create_kernel(program, "lens_distort_lanczos2");
  gd->kernel_lens_distort_lanczos3 = dt_opencl_create_kernel(program, "lens_distort_lanczos3");
  gd->kernel_lens_vignette = dt_opencl_create_kernel(program, "lens_vignette");

  lfDatabase *dt_iop_lensfun_db = lf_db_new();
  gd->db = (void *)dt_iop_lensfun_db;
#if defined(__MACH__) || defined(__APPLE__)
#else
  if(lf_db_load(dt_iop_lensfun_db) != LF_NO_ERROR)
#endif
  {
    char path[1024];
    dt_loc_get_datadir(path, 1024);
    char *c = path + strlen(path);
    for(; c>path && *c != '/'; c--);
    sprintf(c, "/lensfun");
    dt_iop_lensfun_db->HomeDataDir = g_strdup(path);
    if(lf_db_load(dt_iop_lensfun_db) != LF_NO_ERROR)
      fprintf(stderr, "[iop_lens]: could not load lensfun database in `%s'!\n", path);
  }
}


void reload_defaults(dt_iop_module_t *module)
{
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)module->data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  const dt_image_t *img = &module->dev->image_storage;
  // reload image specific stuff
  // get all we can from exif:
  dt_iop_lensfun_params_t tmp;
  g_strlcpy(tmp.lens, _lens_sanitize(img->exif_lens), 52);
  g_strlcpy(tmp.camera, img->exif_model, 52);
  tmp.crop     = img->exif_crop;
  tmp.aperture = img->exif_aperture;
  tmp.focal    = img->exif_focal_length;
  tmp.scale    = 1.0;
  tmp.inverse  = 0;
  tmp.modify_flags = LF_MODIFY_TCA | LF_MODIFY_VIGNETTING |
                     LF_MODIFY_DISTORTION | LF_MODIFY_GEOMETRY | LF_MODIFY_SCALE;
  tmp.distance = img->exif_focus_distance;
  tmp.target_geom = LF_RECTILINEAR;
  tmp.tca_override = 0;
  tmp.tca_r = 1.0;
  tmp.tca_b = 1.0;

  // init crop from db:
  char model[100];  // truncate often complex descriptions.
  g_strlcpy(model, img->exif_model, 100);
  for(char cnt = 0, *c = model; c < model+100 && *c != '\0'; c++) if(*c == ' ') if(++cnt == 2) *c = '\0';
  if(img->exif_maker[0] || model[0])
  {
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfCamera **cam = lf_db_find_cameras_ext(dt_iop_lensfun_db,
                           img->exif_maker, img->exif_model, 0);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if(cam)
    {
      tmp.crop = cam[0]->CropFactor;
      lf_free(cam);
    }
  }

  memcpy(module->params, &tmp, sizeof(dt_iop_lensfun_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_lensfun_params_t));
  module->default_enabled = 0;
}

void init(dt_iop_module_t *module)
{
  module->params = malloc(sizeof(dt_iop_lensfun_params_t));
  module->default_params = malloc(sizeof(dt_iop_lensfun_params_t));
  module->default_enabled = 0;
  module->params_size = sizeof(dt_iop_lensfun_params_t);
  module->gui_data = NULL;
  module->priority = 210; // module order created by iop_dependencies.py, do not edit!
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

void cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)module->data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  lf_db_destroy(dt_iop_lensfun_db);

  dt_opencl_free_kernel(gd->kernel_lens_distort_bilinear);
  dt_opencl_free_kernel(gd->kernel_lens_distort_bicubic);
  dt_opencl_free_kernel(gd->kernel_lens_distort_lanczos2);
  dt_opencl_free_kernel(gd->kernel_lens_distort_lanczos3);
  dt_opencl_free_kernel(gd->kernel_lens_vignette);
  free(module->data);
  module->data = NULL;
}


/// ############################################################
/// gui stuff: inspired by ufraws lensfun tab:


/* simple function to compute the floating-point precision
   which is enough for "normal use". The criteria is to have
   about 3 leading digits after the initial zeros.  */
static int precision (double x, double adj)
{
  x *= adj;

  if (x == 0) return 1;
  if (x < 1.0)
    if (x < 0.1)
      if (x < 0.01)
        return 5;
      else
        return 4;
    else
      return 3;
  else if (x < 100.0)
    if (x < 10.0)
      return 2;
    else
      return 1;
  else
    return 0;
}

/* -- ufraw ptr array functions -- */

static int ptr_array_insert_sorted (
  GPtrArray *array, const void *item, GCompareFunc compare)
{
  int length = array->len;
  g_ptr_array_set_size (array, length + 1);
  const void **root = (const void **)array->pdata;

  int m = 0, l = 0, r = length - 1;

  // Skip trailing NULL, if any
  if (l <= r && !root [r])
    r--;

  while (l <= r)
  {
    m = (l + r) / 2;
    int cmp = compare (root [m], item);

    if (cmp == 0)
    {
      ++m;
      goto done;
    }
    else if (cmp < 0)
      l = m + 1;
    else
      r = m - 1;
  }
  if (r == m)
    m++;

done:
  memmove (root + m + 1, root + m, (length - m) * sizeof (void *));
  root [m] = item;
  return m;
}

static int ptr_array_find_sorted (
  const GPtrArray *array, const void *item, GCompareFunc compare)
{
  int length = array->len;
  void **root = array->pdata;

  int l = 0, r = length - 1;
  int m = 0, cmp = 0;

  if (!length)
    return -1;

  // Skip trailing NULL, if any
  if (!root [r])
    r--;

  while (l <= r)
  {
    m = (l + r) / 2;
    cmp = compare (root [m], item);

    if (cmp == 0)
      return m;
    else if (cmp < 0)
      l = m + 1;
    else
      r = m - 1;
  }

  return -1;
}

static void ptr_array_insert_index (
  GPtrArray *array, const void *item, int index)
{
  const void **root;
  int length = array->len;
  g_ptr_array_set_size (array, length + 1);
  root = (const void **)array->pdata;
  memmove (root + index + 1, root + index, (length - index) * sizeof (void *));
  root [index] = item;
}

/* -- end ufraw ptr array functions -- */

/* -- camera -- */

static void camera_set (dt_iop_module_t *self, const lfCamera *cam)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  gchar *fm;
  const char *maker, *model, *variant;
  char _variant [100];

  if (!cam)
  {
    gtk_button_set_label(GTK_BUTTON(g->camera_model), "");
    gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->camera_model))), PANGO_ELLIPSIZE_END);
    g_object_set(G_OBJECT(g->camera_model), "tooltip-text", "", (char *)NULL);
    return;
  }

  g_strlcpy(p->camera, cam->Model, 52);
  p->crop = cam->CropFactor;
  g->camera = cam;

  maker = lf_mlstr_get (cam->Maker);
  model = lf_mlstr_get (cam->Model);
  variant = lf_mlstr_get (cam->Variant);

  if (model)
  {
    if (maker)
      fm = g_strdup_printf ("%s, %s", maker, model);
    else
      fm = g_strdup_printf ("%s", model);
    gtk_button_set_label (GTK_BUTTON (g->camera_model), fm);
    gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->camera_model))), PANGO_ELLIPSIZE_END);
    g_free (fm);
  }

  if (variant)
    snprintf (_variant, sizeof (_variant), " (%s)", variant);
  else
    _variant [0] = 0;

  fm = g_strdup_printf (_("maker:\t\t%s\n"
                          "model:\t\t%s%s\n"
                          "mount:\t\t%s\n"
                          "crop factor:\t%.1f"),
                        maker, model, _variant,
                        cam->Mount, cam->CropFactor);
  g_object_set(G_OBJECT(g->camera_model), "tooltip-text", fm, (char *)NULL);
  g_free (fm);
}

static void camera_menu_select (
  GtkMenuItem *menuitem, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  camera_set (self, (lfCamera *)g_object_get_data(G_OBJECT(menuitem), "lfCamera"));
  if(!darktable.gui->reset) dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void camera_menu_fill (dt_iop_module_t *self, const lfCamera *const *camlist)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  unsigned i;
  GPtrArray *makers, *submenus;

  if (g->camera_menu)
  {
    gtk_widget_destroy (GTK_WIDGET(g->camera_menu));
    g->camera_menu = NULL;
  }

  /* Count all existing camera makers and create a sorted list */
  makers = g_ptr_array_new ();
  submenus = g_ptr_array_new ();
  for (i = 0; camlist [i]; i++)
  {
    GtkWidget *submenu, *item;
    const char *m = lf_mlstr_get (camlist [i]->Maker);
    int idx = ptr_array_find_sorted (makers, m, (GCompareFunc)g_utf8_collate);
    if (idx < 0)
    {
      /* No such maker yet, insert it into the array */
      idx = ptr_array_insert_sorted (makers, m, (GCompareFunc)g_utf8_collate);
      /* Create a submenu for cameras by this maker */
      submenu = gtk_menu_new ();
      ptr_array_insert_index (submenus, submenu, idx);
    }

    submenu = g_ptr_array_index (submenus, idx);
    /* Append current camera name to the submenu */
    m = lf_mlstr_get (camlist [i]->Model);
    if (!camlist [i]->Variant)
      item = gtk_menu_item_new_with_label (m);
    else
    {
      gchar *fm = g_strdup_printf ("%s (%s)", m, camlist [i]->Variant);
      item = gtk_menu_item_new_with_label (fm);
      g_free (fm);
    }
    gtk_widget_show (item);
    g_object_set_data(G_OBJECT(item), "lfCamera", (void *)camlist [i]);
    g_signal_connect(G_OBJECT(item), "activate",
                     G_CALLBACK(camera_menu_select), self);
    gtk_menu_shell_append (GTK_MENU_SHELL (submenu), item);
  }

  g->camera_menu = GTK_MENU(gtk_menu_new ());
  for (i = 0; i < makers->len; i++)
  {
    GtkWidget *item = gtk_menu_item_new_with_label (g_ptr_array_index (makers, i));
    gtk_widget_show (item);
    gtk_menu_shell_append (GTK_MENU_SHELL (g->camera_menu), item);
    gtk_menu_item_set_submenu (
      GTK_MENU_ITEM (item), (GtkWidget *)g_ptr_array_index (submenus, i));
  }

  g_ptr_array_free (submenus, TRUE);
  g_ptr_array_free (makers, TRUE);
}

static void parse_maker_model (
  const char *txt, char *make, size_t sz_make, char *model, size_t sz_model)
{
  const gchar *sep;

  while (txt [0] && isspace (txt [0]))
    txt++;
  sep = strchr (txt, ',');
  if (sep)
  {
    size_t len = sep - txt;
    if (len > sz_make - 1)
      len = sz_make - 1;
    memcpy (make, txt, len);
    make [len] = 0;

    while (*++sep && isspace (sep [0]))
      ;
    len = strlen (sep);
    if (len > sz_model - 1)
      len = sz_model - 1;
    memcpy (model, sep, len);
    model [len] = 0;
  }
  else
  {
    size_t len = strlen (txt);
    if (len > sz_model - 1)
      len = sz_model - 1;
    memcpy (model, txt, len);
    model [len] = 0;
    make [0] = 0;
  }
}

static void camera_menusearch_clicked(
  GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)self->data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;

  (void)button;

  const lfCamera *const *camlist;
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  camlist = lf_db_get_cameras (dt_iop_lensfun_db);
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  if (!camlist) return;
  camera_menu_fill (self, camlist);
  gtk_menu_popup (GTK_MENU (g->camera_menu), NULL, NULL, NULL, NULL,
                  0, gtk_get_current_event_time ());
}

static void camera_autosearch_clicked(
  GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)self->data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  char make [200], model [200];
  const gchar *txt = ((dt_iop_lensfun_params_t*)self->default_params)->camera;

  (void)button;

  if(txt[0] == '\0')
  {
    const lfCamera *const *camlist;
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    camlist = lf_db_get_cameras (dt_iop_lensfun_db);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if (!camlist) return;
    camera_menu_fill (self, camlist);
  }
  else
  {
    parse_maker_model (txt, make, sizeof (make), model, sizeof (model));
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfCamera **camlist = lf_db_find_cameras_ext (dt_iop_lensfun_db, make, model, 0);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if (!camlist) return;
    camera_menu_fill (self, camlist);
    lf_free (camlist);
  }

  gtk_menu_popup (GTK_MENU (g->camera_menu), NULL, NULL, NULL, NULL,
                  0, gtk_get_current_event_time ());
}

/* -- end camera -- */

static void lens_comboentry_focal_update (GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  const char *text = dt_bauhaus_combobox_get_text(widget);
  if(text)
    (void)sscanf (text, "%f", &p->focal);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void lens_comboentry_aperture_update (GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  const char *text = dt_bauhaus_combobox_get_text(widget);
  if(text)
    (void)sscanf (text, "%f", &p->aperture);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void lens_comboentry_distance_update (GtkWidget *widget, dt_iop_module_t *self)
{
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  const char *text = dt_bauhaus_combobox_get_text(widget);
  if(text)
    (void)sscanf (text, "%f", &p->distance);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void delete_children (GtkWidget *widget, gpointer data)
{
  (void)data;
  gtk_widget_destroy (widget);
}

static void lens_set (dt_iop_module_t *self, const lfLens *lens)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  gchar *fm;
  const char *maker, *model;
  unsigned i;
  gdouble focal_values [] =
  {
    -INFINITY, 4.5, 8, 10, 12, 14, 15, 16, 17, 18, 20, 24, 28, 30, 31, 35, 38, 40, 43,
    45, 50, 55, 60, 70, 75, 77, 80, 85, 90, 100, 105, 110, 120, 135,
    150, 200, 210, 240, 250, 300, 400, 500, 600, 800, 1000, INFINITY
  };
  gdouble aperture_values [] =
  {
    -INFINITY, 0.7, 0.8, 0.9, 1, 1.1, 1.2, 1.4, 1.8, 2, 2.2, 2.5, 2.8, 3.2, 3.4, 4, 4.5, 5.0, 5.6, 6.3,
    7.1, 8, 9, 10, 11, 13, 14, 16, 18, 20, 22, 25, 29, 32, 38, INFINITY
  };

  if (!lens)
  {
    gtk_container_foreach (
      GTK_CONTAINER (g->detection_warning), delete_children, NULL);

    GtkLabel *label;

    label = GTK_LABEL(gtk_label_new(_("camera/lens not found - please select manually")));

    g_object_set (GTK_OBJECT(label), "tooltip-text", _("try to locate your camera/lens in the above two menus"), (char *)NULL);

    gtk_box_pack_start(GTK_BOX(g->detection_warning), GTK_WIDGET(label), FALSE, FALSE, 0);

    gtk_widget_hide(g->lens_param_box);
    gtk_widget_show_all (g->detection_warning);
    return;
  }

  maker = lf_mlstr_get (lens->Maker);
  model = lf_mlstr_get (lens->Model);

  g_strlcpy(p->lens, model, 52);

  if (model)
  {
    if (maker)
      fm = g_strdup_printf ("%s, %s", maker, model);
    else
      fm = g_strdup_printf ("%s", model);
    gtk_button_set_label (GTK_BUTTON (g->lens_model), fm);
    gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->lens_model))), PANGO_ELLIPSIZE_END);
    g_free (fm);
  }

  char focal [100], aperture [100], mounts [200];

  if (lens->MinFocal < lens->MaxFocal)
    snprintf (focal, sizeof (focal), "%g-%gmm", lens->MinFocal, lens->MaxFocal);
  else
    snprintf (focal, sizeof (focal), "%gmm", lens->MinFocal);
  if (lens->MinAperture < lens->MaxAperture)
    snprintf (aperture, sizeof (aperture), "%g-%g", lens->MinAperture, lens->MaxAperture);
  else
    snprintf (aperture, sizeof (aperture), "%g", lens->MinAperture);

  mounts [0] = 0;
  if (lens->Mounts)
    for (i = 0; lens->Mounts [i]; i++)
    {
      if (i > 0)
        g_strlcat (mounts, ", ", sizeof (mounts));
      g_strlcat (mounts, lens->Mounts [i], sizeof (mounts));
    }

  fm = g_strdup_printf (_("maker:\t\t%s\n"
                          "model:\t\t%s\n"
                          "focal range:\t%s\n"
                          "aperture:\t%s\n"
                          "crop factor:\t%.1f\n"
                          "type:\t\t%s\n"
                          "mounts:\t\t%s"),
                        maker ? maker : "?", model ? model : "?",
                        focal, aperture, lens->CropFactor,
                        lf_get_lens_type_desc (lens->Type, NULL), mounts);
  g_object_set(G_OBJECT(g->lens_model), "tooltip-text", fm, (char *)NULL);
  g_free (fm);

  /* Create the focal/aperture/distance combo boxes */
  gtk_container_foreach (
    GTK_CONTAINER (g->lens_param_box), delete_children, NULL);

  int ffi = 1, fli = -1;
  for (i = 1; i < sizeof (focal_values) / sizeof (gdouble) - 1; i++)
  {
    if (focal_values [i] < lens->MinFocal)
      ffi = i + 1;
    if (focal_values [i] > lens->MaxFocal && fli == -1)
      fli = i;
  }
  if (focal_values [ffi] > lens->MinFocal)
  {
    focal_values [ffi - 1] = lens->MinFocal;
    ffi--;
  }
  if (lens->MaxFocal == 0 || fli < 0)
    fli = sizeof (focal_values) / sizeof (gdouble) - 1;
  if (focal_values [fli+1] < lens->MaxFocal)
  {
    focal_values [fli + 1] = lens->MaxFocal;
    ffi++;
  }
  if (fli < ffi)
    fli = ffi + 1;


  GtkWidget *w;
  char txt[30];

  // focal length
  w = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(w, NULL, _("mm"));
  g_object_set(G_OBJECT(w), "tooltip-text", _("focal length (mm)"), (char *)NULL);
  snprintf(txt, sizeof (txt), "%.*f", precision(p->focal, 10.0), p->focal);
  dt_bauhaus_combobox_add(w, txt);
  for(int k=0; k<fli-ffi; k++)
  {
    snprintf(txt, sizeof (txt), "%.*f", precision(focal_values[ffi+k], 10.0), focal_values[ffi+k]);
    dt_bauhaus_combobox_add(w, txt);
  }
  g_signal_connect (G_OBJECT(w), "value-changed",
                    G_CALLBACK(lens_comboentry_focal_update), self);
  gtk_box_pack_start(GTK_BOX(g->lens_param_box), w, TRUE, TRUE, 0);
  dt_bauhaus_combobox_set_editable(w, 1);
  g->cbe[0] = w;


  // f-stop
  ffi = 1, fli = sizeof (aperture_values) / sizeof (gdouble) - 1;
  for (i = 1; i < sizeof (aperture_values) / sizeof (gdouble) - 1; i++)
    if (aperture_values [i] < lens->MinAperture)
      ffi = i + 1;
  if (aperture_values [ffi] > lens->MinAperture)
  {
    aperture_values [ffi - 1] = lens->MinAperture;
    ffi--;
  }

  w = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(w, NULL, _("f/"));
  g_object_set(G_OBJECT(w), "tooltip-text", _("f-number (aperture)"), (char *)NULL);
  snprintf(txt, sizeof (txt), "%.*f", precision(p->aperture, 10.0), p->aperture);
  dt_bauhaus_combobox_add(w, txt);
  for(size_t k=0; k<fli-ffi; k++)
  {
    snprintf(txt, sizeof (txt), "%.*f", precision(aperture_values[ffi+k], 10.0), aperture_values[ffi+k]);
    dt_bauhaus_combobox_add(w, txt);
  }
  g_signal_connect (G_OBJECT(w), "value-changed",
                    G_CALLBACK(lens_comboentry_aperture_update), self);
  gtk_box_pack_start(GTK_BOX(g->lens_param_box), w, TRUE, TRUE, 0);
  dt_bauhaus_combobox_set_editable(w, 1);
  g->cbe[1] = w;

  w = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(w, NULL, _("d"));
  g_object_set(G_OBJECT(w), "tooltip-text", _("distance to subject"), (char *)NULL);
  snprintf(txt, sizeof (txt), "%.*f", precision(p->distance, 10.0), p->distance);
  dt_bauhaus_combobox_add(w, txt);
  float val = 0.25f;
  for(int k=0; k<25; k++)
  {
    if(val > 1000.0f) val = 1000.0f;
    snprintf(txt, sizeof (txt), "%.*f", precision(val, 10.0), val);
    dt_bauhaus_combobox_add(w, txt);
    if(val >= 1000.0f) break;
    val *= sqrtf(2.0f);
  }
  g_signal_connect (G_OBJECT(w), "value-changed",
                    G_CALLBACK(lens_comboentry_distance_update), self);
  gtk_box_pack_start(GTK_BOX(g->lens_param_box), w, TRUE, TRUE, 0);
  dt_bauhaus_combobox_set_editable(w, 1);
  g->cbe[2] = w;

  gtk_widget_hide(g->detection_warning);
  gtk_widget_show_all (g->lens_param_box);
}

static void lens_menu_select (
  GtkMenuItem *menuitem, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  lens_set (self, (lfLens *)g_object_get_data(G_OBJECT(menuitem), "lfLens"));
  if(!darktable.gui->reset) dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void lens_menu_fill (
  dt_iop_module_t *self, const lfLens *const *lenslist)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  unsigned i;
  GPtrArray *makers, *submenus;

  if (g->lens_menu)
  {
    gtk_widget_destroy (GTK_WIDGET(g->lens_menu));
    g->lens_menu = NULL;
  }

  /* Count all existing lens makers and create a sorted list */
  makers = g_ptr_array_new ();
  submenus = g_ptr_array_new ();
  for (i = 0; lenslist [i]; i++)
  {
    GtkWidget *submenu, *item;
    const char *m = lf_mlstr_get (lenslist [i]->Maker);
    int idx = ptr_array_find_sorted (makers, m, (GCompareFunc)g_utf8_collate);
    if (idx < 0)
    {
      /* No such maker yet, insert it into the array */
      idx = ptr_array_insert_sorted (makers, m, (GCompareFunc)g_utf8_collate);
      /* Create a submenu for lenses by this maker */
      submenu = gtk_menu_new ();
      ptr_array_insert_index (submenus, submenu, idx);
    }

    submenu = g_ptr_array_index (submenus, idx);
    /* Append current lens name to the submenu */
    item = gtk_menu_item_new_with_label (lf_mlstr_get (lenslist [i]->Model));
    gtk_widget_show (item);
    g_object_set_data(G_OBJECT(item), "lfLens", (void *)lenslist [i]);
    g_signal_connect(G_OBJECT(item), "activate",
                     G_CALLBACK(lens_menu_select), self);
    gtk_menu_shell_append (GTK_MENU_SHELL (submenu), item);
  }

  g->lens_menu = GTK_MENU(gtk_menu_new ());
  for (i = 0; i < makers->len; i++)
  {
    GtkWidget *item = gtk_menu_item_new_with_label (g_ptr_array_index (makers, i));
    gtk_widget_show (item);
    gtk_menu_shell_append (GTK_MENU_SHELL (g->lens_menu), item);
    gtk_menu_item_set_submenu (
      GTK_MENU_ITEM (item), (GtkWidget *)g_ptr_array_index (submenus, i));
  }

  g_ptr_array_free (submenus, TRUE);
  g_ptr_array_free (makers, TRUE);
}


static void lens_menusearch_clicked(
  GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)self->data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  const lfLens **lenslist;

  (void)button;

  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  lenslist = lf_db_find_lenses_hd (dt_iop_lensfun_db, g->camera,NULL,NULL, 0);
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  if (!lenslist) return;
  lens_menu_fill (self, lenslist);
  lf_free (lenslist);

  gtk_menu_popup (GTK_MENU (g->lens_menu), NULL, NULL, NULL, NULL,
                  0, gtk_get_current_event_time ());
}

static void lens_autosearch_clicked(
  GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)self->data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  const lfLens **lenslist;
  char make [200], model [200];
  const gchar *txt = ((dt_iop_lensfun_params_t*)self->default_params)->lens;

  (void)button;

  parse_maker_model (txt, make, sizeof (make), model, sizeof (model));
  dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
  lenslist = lf_db_find_lenses_hd (dt_iop_lensfun_db, g->camera,
                                   make [0] ? make : NULL,
                                   model [0] ? model : NULL, 0);
  dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  if (!lenslist) return;
  lens_menu_fill (self, lenslist);
  lf_free (lenslist);

  gtk_menu_popup (GTK_MENU (g->lens_menu), NULL, NULL, NULL, NULL,
                  0, gtk_get_current_event_time ());
}

/* -- end lens -- */

static void target_geometry_changed (GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;

  int pos = dt_bauhaus_combobox_get(widget);
  p->target_geom = pos + LF_UNKNOWN + 1;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void modflags_changed (GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  int pos = dt_bauhaus_combobox_get(widget);
  GList *modifiers = g->modifiers;
  while(modifiers)
  {
    // could use g_list_nth. this seems safer?
    dt_iop_lensfun_modifier_t *mm = (dt_iop_lensfun_modifier_t *)modifiers->data;
    if(mm->pos == pos)
    {
      p->modify_flags = (p->modify_flags & ~LENSFUN_MODFLAG_MASK) | mm->modflag;
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      break;
    }
    modifiers = g_list_next(modifiers);
  }
}

static void reverse_toggled(GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  p->inverse = dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void tca_changed(GtkWidget *slider, dt_iop_module_t *self)
{
  dt_iop_lensfun_params_t   *p = (dt_iop_lensfun_params_t   *)self->params;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  const float val = dt_bauhaus_slider_get(slider);
  if(slider == g->tca_r) p->tca_r = val;
  else                   p->tca_b = val;
  if(p->tca_r != 1.0 || p->tca_b != 1.0) p->tca_override = 1;
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static void scale_changed(GtkWidget *slider, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  p->scale = dt_bauhaus_slider_get(slider);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}

static float get_autoscale(dt_iop_module_t *self)
{
  dt_iop_lensfun_params_t   *p = (dt_iop_lensfun_params_t   *)self->params;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)self->data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  float scale = 1.0;
  if(p->lens[0] != '\0')
  {
    char make [200], model [200];
    const gchar *txt = gtk_button_get_label(GTK_BUTTON(g->lens_model));
    parse_maker_model (txt, make, sizeof (make), model, sizeof (model));
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfLens **lenslist = lf_db_find_lenses_hd (dt_iop_lensfun_db, g->camera, NULL, p->lens, 0);
    if(lenslist && !lenslist[1])
    {
      // create dummy modifier
      lfModifier *modifier = lf_modifier_new(lenslist[0], p->crop, self->dev->image_storage.width, self->dev->image_storage.height);
      (void)lf_modifier_initialize(
        modifier, lenslist[0], LF_PF_F32,
        p->focal, p->aperture,
        p->distance, 1.0f,
        p->target_geom, p->modify_flags, p->inverse);
      scale = lf_modifier_get_auto_scale (modifier, p->inverse);
      lf_modifier_destroy(modifier);
    }
    lf_free (lenslist);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }
  return scale;
}

static void autoscale_pressed(GtkWidget *button, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  const float scale = get_autoscale(self);
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  dt_bauhaus_slider_set(g->scale, scale);
}


static gboolean
expose (GtkWidget *widget, GdkEventExpose *event, dt_iop_module_t *self)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  if(darktable.gui->reset) return FALSE;

  if(g->corrections_done == -1) return FALSE;

  char *message = "";
  GList *modifiers = g->modifiers;
  while(modifiers)
  {
    // could use g_list_nth. this seems safer?
    dt_iop_lensfun_modifier_t *mm = (dt_iop_lensfun_modifier_t *)modifiers->data;
    if(mm->modflag == g->corrections_done)
    {
      message = mm->name;
      break;
    }
    modifiers = g_list_next(modifiers);
  }

  g->corrections_done = -1;

  darktable.gui->reset = 1;
  gtk_label_set_text(g->message, message);
  darktable.gui->reset = 0;

  return FALSE;
}


void gui_init(struct dt_iop_module_t *self)
{
  self->gui_data = malloc(sizeof(dt_iop_lensfun_gui_data_t));
  // dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)self->data;
  // lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  g->camera = NULL;
  g->camera_menu = NULL;
  g->lens_menu = NULL;
  g->modifiers = NULL;
  g->corrections_done = -1;


  // initialize modflags options
  int pos = -1;
  dt_iop_lensfun_modifier_t *modifier;
  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("none"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_NONE;
  modifier->pos = ++pos;

  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("all"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_ALL;
  modifier->pos = ++pos;

  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("distortion & TCA"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_DIST_TCA;
  modifier->pos = ++pos;

  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("distortion & vignetting"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_DIST_VIGN;
  modifier->pos = ++pos;

  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("TCA & vignetting"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_TCA_VIGN;
  modifier->pos = ++pos;

  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("only distortion"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_DIST;
  modifier->pos = ++pos;

  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("only TCA"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_TCA;
  modifier->pos = ++pos;

  modifier = (dt_iop_lensfun_modifier_t *)g_malloc0(sizeof(dt_iop_lensfun_modifier_t));
  dt_utf8_strlcpy(modifier->name, _("only vignetting"), sizeof(modifier->name));
  g->modifiers = g_list_append(g->modifiers, modifier);
  modifier->modflag = LENSFUN_MODFLAG_VIGN;
  modifier->pos = ++pos;

  GtkWidget *button;

  self->widget = gtk_vbox_new(TRUE, DT_BAUHAUS_SPACE);

  g_signal_connect(G_OBJECT(self->widget), "expose-event", G_CALLBACK(expose), self);

  // camera selector
  GtkWidget *hbox = gtk_hbox_new(FALSE, 0);
  g->camera_model = GTK_BUTTON(dtgtk_button_new(NULL, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER));
  dt_gui_key_accel_block_on_focus_connect (GTK_WIDGET (g->camera_model));
  gtk_button_set_label(g->camera_model, self->dev->image_storage.exif_model);
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->camera_model))), PANGO_ELLIPSIZE_END);
  g_signal_connect (G_OBJECT (g->camera_model), "clicked",
                    G_CALLBACK (camera_menusearch_clicked), self);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->camera_model), TRUE, TRUE, 0);
  button = dtgtk_button_new(dtgtk_cairo_paint_solid_triangle, CPF_STYLE_FLAT|CPF_DIRECTION_DOWN);
  g->find_camera_button = button;
  gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);
  g_object_set(G_OBJECT(button), "tooltip-text", _("find camera"), (char *)NULL);
  g_signal_connect (G_OBJECT (button), "clicked",
                    G_CALLBACK (camera_autosearch_clicked), self);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);

  // lens selector
  hbox = gtk_hbox_new(FALSE, 0);
  g->lens_model = GTK_BUTTON(dtgtk_button_new(NULL, CPF_STYLE_FLAT|CPF_DO_NOT_USE_BORDER));
  dt_gui_key_accel_block_on_focus_connect (GTK_WIDGET (g->lens_model));
  gtk_button_set_label(g->lens_model, self->dev->image_storage.exif_lens);
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->lens_model))), PANGO_ELLIPSIZE_END);
  g_signal_connect (G_OBJECT (g->lens_model), "clicked",
                    G_CALLBACK (lens_menusearch_clicked), self);
  gtk_box_pack_start(GTK_BOX(hbox), GTK_WIDGET(g->lens_model), TRUE, TRUE, 0);
  button = dtgtk_button_new(dtgtk_cairo_paint_solid_triangle, CPF_STYLE_FLAT|CPF_DIRECTION_DOWN);
  g->find_lens_button = GTK_WIDGET(button);
  gtk_box_pack_start(GTK_BOX(hbox), button, FALSE, FALSE, 0);
  g_object_set(G_OBJECT(button), "tooltip-text", _("find lens"), (char *)NULL);
  g_signal_connect (G_OBJECT (button), "clicked",
                    G_CALLBACK (lens_autosearch_clicked), self);
  gtk_box_pack_start(GTK_BOX(self->widget), hbox, TRUE, TRUE, 0);


  // lens properties
  g->lens_param_box = gtk_hbox_new(FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->lens_param_box, TRUE, TRUE, 0);

  // camera/lens not detected warning box
  g->detection_warning = gtk_hbox_new(FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), g->detection_warning, TRUE, TRUE, 0);

#if 0
  // if unambiguous info is there, use it.
  if(self->dev->image_storage.exif_lens[0] != '\0')
  {
    char make [200], model [200];
    const gchar *txt = gtk_entry_get_text(GTK_ENTRY(g->lens_model));
    parse_maker_model (txt, make, sizeof (make), model, sizeof (model));
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfLens **lenslist = lf_db_find_lenses_hd (dt_iop_lensfun_db, g->camera,
                              make [0] ? make : NULL,
                              model [0] ? model : NULL, 0);
    if(lenslist && !lenslist[1]) lens_set (self, lenslist[0]);
    lf_free (lenslist);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }
#endif

  // selector for correction type (modflags): one or more out of distortion, TCA, vignetting
  g->modflags = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->modflags, NULL, _("corrections"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->modflags, TRUE, TRUE, 0);
  g_object_set (GTK_OBJECT(g->modflags), "tooltip-text", _("which corrections to apply"), (char *)NULL);
  GList *l = g->modifiers;
  while(l)
  {
    dt_iop_lensfun_modifier_t *modifier = (dt_iop_lensfun_modifier_t *)l->data;
    dt_bauhaus_combobox_add(g->modflags, modifier->name);
    l = g_list_next(l);
  }
  dt_bauhaus_combobox_set(g->modflags, 0);
  g_signal_connect (G_OBJECT (g->modflags), "value-changed",
                    G_CALLBACK (modflags_changed),
                    (gpointer)self);

  // target geometry
  g->target_geom = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->target_geom, NULL, _("geometry"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->target_geom, TRUE, TRUE, 0);
  g_object_set (GTK_OBJECT(g->target_geom), "tooltip-text", _("target geometry"), (char *)NULL);
  dt_bauhaus_combobox_add(g->target_geom, _("rectilinear"));
  dt_bauhaus_combobox_add(g->target_geom, _("fish-eye"));
  dt_bauhaus_combobox_add(g->target_geom, _("panoramic"));
  dt_bauhaus_combobox_add(g->target_geom, _("equirectangular"));
#if LF_VERSION >= ((0 << 24) | (2 << 16) | (6 << 8) | 0)
  dt_bauhaus_combobox_add(g->target_geom, _("orthographic"));
  dt_bauhaus_combobox_add(g->target_geom, _("stereographic"));
  dt_bauhaus_combobox_add(g->target_geom, _("equisolid angle"));
  dt_bauhaus_combobox_add(g->target_geom, _("thoby fish-eye"));
#endif
  g_signal_connect (G_OBJECT (g->target_geom), "value-changed",
                    G_CALLBACK (target_geometry_changed),
                    (gpointer)self);

  // scale
  g->scale = dt_bauhaus_slider_new_with_range(self, 0.1, 2.0, 0.005, p->scale, 3);
  g_object_set (GTK_OBJECT(g->scale), "tooltip-text", _("auto scale"), (char *)NULL);
  dt_bauhaus_widget_set_label(g->scale, NULL, _("scale"));
  g_signal_connect (G_OBJECT (g->scale), "value-changed",
                    G_CALLBACK (scale_changed), self);
  g_signal_connect (G_OBJECT (g->scale), "quad-pressed", G_CALLBACK (autoscale_pressed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->scale, TRUE, TRUE, 0);
  dt_bauhaus_widget_set_quad_paint(g->scale, dtgtk_cairo_paint_refresh, 0);

  // reverse direction
  g->reverse = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->reverse, NULL, _("mode"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->reverse, TRUE, TRUE, 0);
  g_object_set (GTK_OBJECT(g->reverse), "tooltip-text", _("correct distortions or apply them"), (char *)NULL);
  dt_bauhaus_combobox_add(g->reverse, _("correct"));
  dt_bauhaus_combobox_add(g->reverse, _("distort"));
  g_signal_connect (G_OBJECT (g->reverse), "value-changed",
                    G_CALLBACK (reverse_toggled),
                    (gpointer)self);

  // override linear tca (if not 1.0):
  g->tca_r = dt_bauhaus_slider_new_with_range(self, 0.99, 1.01, 0.0001, p->tca_r, 5);
  g_object_set (GTK_OBJECT(g->tca_r), "tooltip-text", _("Transversal Chromatic Aberration red"), (char *)NULL);
  dt_bauhaus_widget_set_label(g->tca_r, NULL, _("TCA red"));
  g_signal_connect (G_OBJECT (g->tca_r), "value-changed", G_CALLBACK (tca_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->tca_r, TRUE, TRUE, 0);

  g->tca_b = dt_bauhaus_slider_new_with_range(self, 0.99, 1.01, 0.0001, p->tca_b, 5);
  g_object_set (GTK_OBJECT(g->tca_b), "tooltip-text", _("Transversal Chromatic Aberration blue"), (char *)NULL);
  dt_bauhaus_widget_set_label(g->tca_b, NULL, _("TCA blue"));
  g_signal_connect (G_OBJECT (g->tca_b), "value-changed", G_CALLBACK (tca_changed), self);
  gtk_box_pack_start(GTK_BOX(self->widget), g->tca_b, TRUE, TRUE, 0);

  // message box to inform user what corrections have been done. this is useful as depending on lensfuns
  // profile only some of the lens flaws can be corrected
  GtkHBox *hbox1 = GTK_HBOX(gtk_hbox_new(FALSE, 0));
  GtkLabel *label = GTK_LABEL(gtk_label_new(_("corrections done: ")));
  g_object_set (GTK_OBJECT(label), "tooltip-text", _("which corrections have actually been done"), (char *)NULL);
  gtk_box_pack_start(GTK_BOX(hbox1), GTK_WIDGET(label), FALSE, FALSE, 0);
  g->message = GTK_LABEL(gtk_label_new ("")); // This gets filled in by process
  gtk_box_pack_start(GTK_BOX(hbox1), GTK_WIDGET(g->message), FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(self->widget), GTK_WIDGET(hbox1), TRUE, TRUE, 0);

}



void gui_update(struct dt_iop_module_t *self)
{
  // let gui elements reflect params
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  dt_iop_lensfun_params_t *p = (dt_iop_lensfun_params_t *)self->params;
  dt_iop_lensfun_global_data_t *gd = (dt_iop_lensfun_global_data_t *)self->data;
  lfDatabase *dt_iop_lensfun_db = (lfDatabase *)gd->db;
  gtk_button_set_label(g->camera_model, p->camera);
  gtk_button_set_label(g->lens_model, p->lens);
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->camera_model))), PANGO_ELLIPSIZE_END);
  gtk_label_set_ellipsize(GTK_LABEL(gtk_bin_get_child(GTK_BIN(g->lens_model))), PANGO_ELLIPSIZE_END);
  g_object_set(G_OBJECT(g->camera_model), "tooltip-text", "", (char *)NULL);
  g_object_set(G_OBJECT(g->lens_model), "tooltip-text", "", (char *)NULL);

  g->corrections_done = -1;
  gtk_label_set_text(g->message, "");

  int modflag = p->modify_flags & LENSFUN_MODFLAG_MASK;
  GList *modifiers = g->modifiers;
  while(modifiers)
  {
    // could use g_list_nth. this seems safer?
    dt_iop_lensfun_modifier_t *mm = (dt_iop_lensfun_modifier_t *)modifiers->data;
    if(mm->modflag == modflag)
    {
      dt_bauhaus_combobox_set(g->modflags, mm->pos);
      break;
    }
    modifiers = g_list_next(modifiers);
  }

  dt_bauhaus_combobox_set(g->target_geom, p->target_geom - LF_UNKNOWN - 1);
  dt_bauhaus_combobox_set(g->reverse, p->inverse);
  dt_bauhaus_slider_set(g->tca_r, p->tca_r);
  dt_bauhaus_slider_set(g->tca_b, p->tca_b);
  dt_bauhaus_slider_set(g->scale, p->scale);
  const lfCamera **cam = NULL;
  g->camera = NULL;
  if(p->camera[0])
  {
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    cam = lf_db_find_cameras_ext(dt_iop_lensfun_db,
                                 NULL, p->camera, 0);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
    if(cam) g->camera = cam[0];
  }
  if(g->camera && p->lens[0])
  {
    char make [200], model [200];
    const gchar *txt = gtk_button_get_label(GTK_BUTTON(g->lens_model));
    parse_maker_model (txt, make, sizeof (make), model, sizeof (model));
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    const lfLens **lenslist = lf_db_find_lenses_hd (dt_iop_lensfun_db, g->camera,
                              make [0] ? make : NULL,
                              model [0] ? model : NULL, 0);
    if(lenslist) lens_set (self, lenslist[0]);
    else         lens_set (self, NULL);
    lf_free (lenslist);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }
  else
  {
    dt_pthread_mutex_lock(&darktable.plugin_threadsafe);
    lens_set (self, NULL);
    dt_pthread_mutex_unlock(&darktable.plugin_threadsafe);
  }
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_lensfun_gui_data_t *g = (dt_iop_lensfun_gui_data_t *)self->gui_data;
  dt_gui_key_accel_block_on_focus_disconnect (GTK_WIDGET (g->lens_model));
  dt_gui_key_accel_block_on_focus_disconnect (GTK_WIDGET (g->camera_model));
  while(g->modifiers)
  {
    g_free(g->modifiers->data);
    g->modifiers = g_list_delete_link(g->modifiers, g->modifiers);
  }

  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
