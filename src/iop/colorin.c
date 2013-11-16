/*
    This file is part of darktable,
    copyright (c) 2009--2011 johannes hanika.
    copyright (c) 2011 henrik andersson

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
#include "iop/colorin.h"
#include "develop/develop.h"
#include "control/control.h"
#include "gui/gtk.h"
#include "bauhaus/bauhaus.h"
#include "common/colorspaces.h"
#include "common/colormatrices.c"
#include "common/opencl.h"
#include "common/image_cache.h"
#ifdef HAVE_OPENJPEG
#include "common/imageio_j2k.h"
#endif
#include "common/imageio_jpeg.h"
#include "external/adobe_coeff.c"
#include <xmmintrin.h>
#include <stdlib.h>
#include <math.h>
#include <assert.h>
#include <string.h>


DT_MODULE(1)

static void update_profile_list(dt_iop_module_t *self);


typedef struct dt_iop_colorin_global_data_t
{
  int kernel_colorin;
}
dt_iop_colorin_global_data_t;


const char *
name()
{
  return _("input color profile");
}

int
groups ()
{
  return IOP_GROUP_COLOR;
}

int
flags ()
{
  return IOP_FLAGS_ALLOW_TILING | IOP_FLAGS_ONE_INSTANCE;
}

void
init_global(dt_iop_module_so_t *module)
{
  const int program = 2; // basic.cl, from programs.conf
  dt_iop_colorin_global_data_t *gd = (dt_iop_colorin_global_data_t *)malloc(sizeof(dt_iop_colorin_global_data_t));
  module->data = gd;
  gd->kernel_colorin = dt_opencl_create_kernel(program, "colorin");
}

void
cleanup_global(dt_iop_module_so_t *module)
{
  dt_iop_colorin_global_data_t *gd = (dt_iop_colorin_global_data_t *)module->data;
  dt_opencl_free_kernel(gd->kernel_colorin);
  free(module->data);
  module->data = NULL;
}

#if 0
static void intent_changed (GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;
  p->intent = (dt_iop_color_intent_t)dt_bauhaus_combobox_get(widget);
  dt_dev_add_history_item(darktable.develop, self, TRUE);
}
#endif

static void
profile_changed (GtkWidget *widget, gpointer user_data)
{
  dt_iop_module_t *self = (dt_iop_module_t *)user_data;
  if(self->dt->gui->reset) return;
  dt_iop_request_focus(self);
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)self->params;
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  int pos = dt_bauhaus_combobox_get(widget);
  GList *prof;
  if(pos < g->n_image_profiles)
    prof = g->image_profiles;
  else
  {
    prof = g->global_profiles;
    pos -= g->n_image_profiles;
  }
  while(prof)
  {
    // could use g_list_nth. this seems safer?
    dt_iop_color_profile_t *pp = (dt_iop_color_profile_t *)prof->data;
    if(pp->pos == pos)
    {
      g_strlcpy(p->iccprofile, pp->filename, sizeof(p->iccprofile));
      dt_dev_add_history_item(darktable.develop, self, TRUE);
      return;
    }
    prof = g_list_next(prof);
  }
  // should really never happen.
  fprintf(stderr, "[colorin] color profile %s seems to have disappeared!\n", p->iccprofile);
}

static float
lerp_lut(const float *const lut, const float v)
{
  // TODO: check if optimization is worthwhile!
  const float ft = CLAMPS(v*(LUT_SAMPLES-1), 0, LUT_SAMPLES-1);
  const int t = ft < LUT_SAMPLES-2 ? ft : LUT_SAMPLES-2;
  const float f = ft - t;
  const float l1 = lut[t];
  const float l2 = lut[t+1];
  return l1*(1.0f-f) + l2*f;
}

#ifdef HAVE_OPENCL
int
process_cl (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, cl_mem dev_in, cl_mem dev_out, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  dt_iop_colorin_global_data_t *gd = (dt_iop_colorin_global_data_t *)self->data;
  cl_mem dev_m = NULL, dev_r = NULL, dev_g = NULL, dev_b = NULL, dev_coeffs = NULL;

  cl_int err = -999;
  const int map_blues = piece->pipe->image.flags & DT_IMAGE_RAW;
  const int devid = piece->pipe->devid;
  const int width = roi_in->width;
  const int height = roi_in->height;

  size_t sizes[] = { ROUNDUPWD(width), ROUNDUPHT(height), 1};
  dev_m = dt_opencl_copy_host_to_device_constant(devid, sizeof(float)*9, d->cmatrix);
  if (dev_m == NULL) goto error;
  dev_r = dt_opencl_copy_host_to_device(devid, d->lut[0], 256, 256, sizeof(float));
  if (dev_r == NULL) goto error;
  dev_g = dt_opencl_copy_host_to_device(devid, d->lut[1], 256, 256, sizeof(float));
  if (dev_g == NULL) goto error;
  dev_b = dt_opencl_copy_host_to_device(devid, d->lut[2], 256, 256, sizeof(float));
  if (dev_b == NULL) goto error;
  dev_coeffs = dt_opencl_copy_host_to_device_constant(devid, sizeof(float)*3*3, (float *)d->unbounded_coeffs);
  if (dev_coeffs == NULL) goto error;
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorin, 0, sizeof(cl_mem), (void *)&dev_in);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorin, 1, sizeof(cl_mem), (void *)&dev_out);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorin, 2, sizeof(int), (void *)&width);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorin, 3, sizeof(int), (void *)&height);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorin, 4, sizeof(cl_mem), (void *)&dev_m);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorin, 5, sizeof(cl_mem), (void *)&dev_r);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorin, 6, sizeof(cl_mem), (void *)&dev_g);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorin, 7, sizeof(cl_mem), (void *)&dev_b);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorin, 8, sizeof(cl_int), (void *)&map_blues);
  dt_opencl_set_kernel_arg(devid, gd->kernel_colorin, 9, sizeof(cl_mem), (void *)&dev_coeffs);
  err = dt_opencl_enqueue_kernel_2d(devid, gd->kernel_colorin, sizes);
  if(err != CL_SUCCESS) goto error;
  dt_opencl_release_mem_object(dev_m);
  dt_opencl_release_mem_object(dev_r);
  dt_opencl_release_mem_object(dev_g);
  dt_opencl_release_mem_object(dev_b);
  dt_opencl_release_mem_object(dev_coeffs);
  return TRUE;

error:
  if (dev_m != NULL) dt_opencl_release_mem_object(dev_m);
  if (dev_r != NULL) dt_opencl_release_mem_object(dev_r);
  if (dev_g != NULL) dt_opencl_release_mem_object(dev_g);
  if (dev_b != NULL) dt_opencl_release_mem_object(dev_b);
  if (dev_coeffs != NULL) dt_opencl_release_mem_object(dev_coeffs);
  dt_print(DT_DEBUG_OPENCL, "[opencl_colorin] couldn't enqueue kernel! %d\n", err);
  return FALSE;
}
#endif

static inline __m128
lab_f_m(const __m128 x)
{
  const __m128 epsilon = _mm_set1_ps(216.0f/24389.0f);
  const __m128 kappa   = _mm_set1_ps(24389.0f/27.0f);

  // calculate as if x > epsilon : result = cbrtf(x)
  // approximate cbrtf(x):
  const __m128 a = _mm_castsi128_ps(_mm_add_epi32(_mm_cvtps_epi32(_mm_div_ps(_mm_cvtepi32_ps(_mm_castps_si128(x)),_mm_set1_ps(3.0f))),_mm_set1_epi32(709921077)));
  const __m128 a3 = _mm_mul_ps(_mm_mul_ps(a,a),a);
  const __m128 res_big = _mm_div_ps(_mm_mul_ps(a,_mm_add_ps(a3,_mm_add_ps(x,x))),_mm_add_ps(_mm_add_ps(a3,a3),x));

  // calculate as if x <= epsilon : result = (kappa*x+16)/116
  const __m128 res_small = _mm_div_ps(_mm_add_ps(_mm_mul_ps(kappa,x),_mm_set1_ps(16.0f)),_mm_set1_ps(116.0f));

  // blend results according to whether each component is > epsilon or not
  const __m128 mask = _mm_cmpgt_ps(x,epsilon);
  return _mm_or_ps(_mm_and_ps(mask,res_big),_mm_andnot_ps(mask,res_small));
}

static inline __m128
dt_XYZ_to_Lab_SSE(const __m128 XYZ)
{
  const __m128 d50_inv  = _mm_set_ps(0.0f, 1.0f/0.8249f, 1.0f, 1.0f/0.9642f);
  const __m128 coef = _mm_set_ps(0.0f,200.0f,500.0f,116.0f);
  const __m128 f = lab_f_m(_mm_mul_ps(XYZ,d50_inv));
  // because d50_inv.z is 0.0f, lab_f(0) == 16/116, so Lab[0] = 116*f[0] - 16 equal to 116*(f[0]-f[3])
  return _mm_mul_ps(coef,_mm_sub_ps(_mm_shuffle_ps(f,f,_MM_SHUFFLE(3,1,0,1)),_mm_shuffle_ps(f,f,_MM_SHUFFLE(3,2,1,3))));
}

void process (struct dt_iop_module_t *self, dt_dev_pixelpipe_iop_t *piece, void *i, void *o, const dt_iop_roi_t *roi_in, const dt_iop_roi_t *roi_out)
{
  const dt_iop_colorin_data_t *const d = (dt_iop_colorin_data_t *)piece->data;
  const float *const mat = d->cmatrix;
  float *in  = (float *)i;
  float *out = (float *)o;
  const int ch = piece->colors;
  const int map_blues = piece->pipe->image.flags & DT_IMAGE_RAW;

  if(mat[0] != -666.0f)
  {
    // only color matrix. use our optimized fast path!
#ifdef _OPENMP
    #pragma omp parallel for default(none) shared(roi_in,roi_out, out, in) schedule(static)
#endif
    for(int j=0; j<roi_out->height; j++)
    {

      float *buf_in  = in + ch*roi_in->width *j;
      float *buf_out = out + ch*roi_out->width*j;
      float cam[3];
      const __m128 m0 = _mm_set_ps(0.0f,mat[6],mat[3],mat[0]);
      const __m128 m1 = _mm_set_ps(0.0f,mat[7],mat[4],mat[1]);
      const __m128 m2 = _mm_set_ps(0.0f,mat[8],mat[5],mat[2]);

      for(int i=0; i<roi_out->width; i++, buf_in+=ch, buf_out+=ch )
      {

        // memcpy(cam, buf_in, sizeof(float)*3);
        // avoid calling this for linear profiles (marked with negative entries), assures unbounded
        // color management without extrapolation.
        for(int i=0; i<3; i++) cam[i] = (d->lut[i][0] >= 0.0f) ?
                                          ((buf_in[i] < 1.0f) ? lerp_lut(d->lut[i], buf_in[i])
                                           : dt_iop_eval_exp(d->unbounded_coeffs[i], buf_in[i]))
                                            : buf_in[i];

        const float YY = cam[0]+cam[1]+cam[2];
        if(map_blues && YY > 0.0f)
        {
          // manual gamut mapping. these values cause trouble when converting back from Lab to sRGB.
          // deeply saturated blues turn into purple fringes, so dampen them before conversion.
          // this is off for non-raw images, which don't seem to have this problem.
          // might be caused by too loose clipping bounds during highlight clipping?
          const float zz = cam[2]/YY;
          // lower amount and higher bound_z make the effect smaller.
          // the effect is weakened the darker input values are, saturating at bound_Y
          const float bound_z = 0.5f, bound_Y = 0.8f;
          const float amount = 0.11f;
          if (zz > bound_z)
          {
            const float t = (zz - bound_z)/(1.0f-bound_z) * fminf(1.0f, YY/bound_Y);
            cam[1] += t*amount;
            cam[2] -= t*amount;
          }
        }

#if 0
        __attribute__((aligned(16))) float XYZ[4];
        _mm_store_ps(XYZ,_mm_add_ps(_mm_add_ps( _mm_mul_ps(m0,_mm_set1_ps(cam[0])), _mm_mul_ps(m1,_mm_set1_ps(cam[1]))), _mm_mul_ps(m2,_mm_set1_ps(cam[2]))));
        dt_XYZ_to_Lab(XYZ, buf_out);
#endif
        __m128 xyz = _mm_add_ps(_mm_add_ps( _mm_mul_ps(m0,_mm_set1_ps(cam[0])), _mm_mul_ps(m1,_mm_set1_ps(cam[1]))), _mm_mul_ps(m2,_mm_set1_ps(cam[2])));
        _mm_stream_ps(buf_out,dt_XYZ_to_Lab_SSE(xyz));
      }
    }
    _mm_sfence();
  }
  else
  {
    // use general lcms2 fallback
    int rowsize=roi_out->width*3;
    float cam[rowsize];
    float Lab[rowsize];

    // FIXME: for some unapparent reason even this breaks lcms2 :(
#if 0//def _OPENMP
    #pragma omp parallel for default(none) shared(roi_out, out, in, d, cam, Lab, rowsize) schedule(static)
#endif
    for(int k=0; k<roi_out->height; k++)
    {
      const int m=(k*(roi_out->width*ch));

      for (int l=0; l<roi_out->width; l++)
      {
        int ci=3*l, ii=ch*l;

        cam[ci+0] = in[m+ii+0];
        cam[ci+1] = in[m+ii+1];
        cam[ci+2] = in[m+ii+2];

        const float YY = cam[ci+0]+cam[ci+1]+cam[ci+2];
        const float zz = cam[ci+2]/YY;
        const float bound_z = 0.5f, bound_Y = 0.5f;
        const float amount = 0.11f;
        if (zz > bound_z)
        {
          const float t = (zz - bound_z)/(1.0f-bound_z) * fminf(1.0, YY/bound_Y);
          cam[ci+1] += t*amount;
          cam[ci+2] -= t*amount;
        }
      }
      // convert to (L,a/L,b/L) to be able to change L without changing saturation.
      // lcms is not thread safe, so work on one copy for each thread :(
      cmsDoTransform (d->xform[dt_get_thread_num()], cam, Lab, roi_out->width);

      for (int l=0; l<roi_out->width; l++)
      {
        int li=3*l, oi=ch*l;
        out[m+oi+0] = Lab[li+0];
        out[m+oi+1] = Lab[li+1];
        out[m+oi+2] = Lab[li+2];
      }
    }
  }

  if(piece->pipe->mask_display)
    dt_iop_alpha_copy(i, o, roi_out->width, roi_out->height);
}

void commit_params (struct dt_iop_module_t *self, dt_iop_params_t *p1, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)p1;
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  if(d->input) cmsCloseProfile(d->input);
  const int num_threads = dt_get_num_threads();
  d->input = NULL;
  for(int t=0; t<num_threads; t++) if(d->xform[t])
    {
      cmsDeleteTransform(d->xform[t]);
      d->xform[t] = NULL;
    }
  d->cmatrix[0] = -666.0f;
  d->lut[0][0] = -1.0f;
  d->lut[1][0] = -1.0f;
  d->lut[2][0] = -1.0f;
  piece->process_cl_ready = 1;
  char datadir[DT_MAX_PATH_LEN];
  char filename[DT_MAX_PATH_LEN];
  dt_loc_get_datadir(datadir, DT_MAX_PATH_LEN);
  if(!strcmp(p->iccprofile, "Lab"))
  {
    piece->enabled = 0;
    return;
  }
  piece->enabled = 1;
  if(!strcmp(p->iccprofile, "darktable"))
  {
    char makermodel[1024];
    dt_colorspaces_get_makermodel(makermodel, 1024, pipe->image.exif_maker, pipe->image.exif_model);
    d->input = dt_colorspaces_create_darktable_profile(makermodel);
    if(!d->input) sprintf(p->iccprofile, "eprofile");
  }
  if(!strcmp(p->iccprofile, "vendor"))
  {
    char makermodel[1024];
    dt_colorspaces_get_makermodel(makermodel, 1024, pipe->image.exif_maker, pipe->image.exif_model);
    d->input = dt_colorspaces_create_vendor_profile(makermodel);
    if(!d->input) sprintf(p->iccprofile, "eprofile");
  }
  if(!strcmp(p->iccprofile, "alternate"))
  {
    char makermodel[1024];
    dt_colorspaces_get_makermodel(makermodel, 1024, pipe->image.exif_maker, pipe->image.exif_model);
    d->input = dt_colorspaces_create_alternate_profile(makermodel);
    if(!d->input) sprintf(p->iccprofile, "eprofile");
  }
  if(!strcmp(p->iccprofile, "eprofile"))
  {
    // embedded color profile
    const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, pipe->image.id);
    if(cimg == NULL || cimg->profile == NULL) sprintf(p->iccprofile, "ematrix");
    else d->input = cmsOpenProfileFromMem(cimg->profile, cimg->profile_size);
    dt_image_cache_read_release(darktable.image_cache, cimg);
  }
  if(!strcmp(p->iccprofile, "ematrix"))
  {
    // embedded matrix, hopefully D65
    if(isnan(pipe->image.d65_color_matrix[0])) sprintf(p->iccprofile, "cmatrix");
    else d->input = dt_colorspaces_create_xyzimatrix_profile((float (*)[3])pipe->image.d65_color_matrix);
  }
  if(!strcmp(p->iccprofile, "cmatrix"))
  {
    // color matrix
    char makermodel[1024];
    dt_colorspaces_get_makermodel(makermodel, 1024, pipe->image.exif_maker, pipe->image.exif_model);
    float cam_xyz[12];
    cam_xyz[0] = -666.0f;
    dt_dcraw_adobe_coeff(makermodel, "", (float (*)[12])cam_xyz);
    if(cam_xyz[0] == -666.0f) sprintf(p->iccprofile, "linear_rgb");
    else d->input = dt_colorspaces_create_xyzimatrix_profile((float (*)[3])cam_xyz);
  }
  if(!strcmp(p->iccprofile, "sRGB"))
  {
    d->input = dt_colorspaces_create_srgb_profile();
  }
  else if(!strcmp(p->iccprofile, "infrared"))
  {
    d->input = dt_colorspaces_create_linear_infrared_profile();
  }
  else if(!strcmp(p->iccprofile, "XYZ"))
  {
    d->input = dt_colorspaces_create_xyz_profile();
  }
  else if(!strcmp(p->iccprofile, "adobergb"))
  {
    d->input = dt_colorspaces_create_adobergb_profile();
  }
  else if(!strcmp(p->iccprofile, "linear_rgb"))
  {
    d->input = dt_colorspaces_create_linear_rgb_profile();
  }
  else if(!d->input)
  {
    dt_colorspaces_find_profile(filename, DT_MAX_PATH_LEN, p->iccprofile, "in");
    d->input = cmsOpenProfileFromFile(filename, "r");
  }
  if(d->input)
  {
    if(dt_colorspaces_get_matrix_from_input_profile (d->input, d->cmatrix, d->lut[0], d->lut[1], d->lut[2], LUT_SAMPLES))
    {
      piece->process_cl_ready = 0;
      d->cmatrix[0] = -666.0f;
      for(int t=0; t<num_threads; t++) d->xform[t] = cmsCreateTransform(d->input, TYPE_RGB_FLT, d->Lab, TYPE_Lab_FLT, p->intent, 0);
    }
  }
  else
  {
    if(strcmp(p->iccprofile, "sRGB"))
    {
      // use linear_rgb as fallback for missing profiles:
      d->input = dt_colorspaces_create_linear_rgb_profile();
    }
    if(!d->input) d->input = dt_colorspaces_create_srgb_profile();
    if(dt_colorspaces_get_matrix_from_input_profile (d->input, d->cmatrix, d->lut[0], d->lut[1], d->lut[2], LUT_SAMPLES))
    {
      piece->process_cl_ready = 0;
      d->cmatrix[0] = -666.0f;
      for(int t=0; t<num_threads; t++) d->xform[t] = cmsCreateTransform(d->input, TYPE_RGB_FLT, d->Lab, TYPE_Lab_FLT, p->intent, 0);
    }
  }
  // user selected a non-supported output profile, check that:
  if(!d->xform[0] && d->cmatrix[0] == -666.0f)
  {
    dt_control_log(_("unsupported input profile has been replaced by linear Rec709 RGB!"));
    if(d->input) dt_colorspaces_cleanup_profile(d->input);
    d->input = dt_colorspaces_create_linear_rgb_profile();
    if(dt_colorspaces_get_matrix_from_input_profile (d->input, d->cmatrix, d->lut[0], d->lut[1], d->lut[2], LUT_SAMPLES))
    {
      piece->process_cl_ready = 0;
      d->cmatrix[0] = -666.0f;
      for(int t=0; t<num_threads; t++) d->xform[t] = cmsCreateTransform(d->Lab, TYPE_RGB_FLT, d->input, TYPE_Lab_FLT, p->intent, 0);
    }
  }

  // now try to initialize unbounded mode:
  // we do a extrapolation for input values above 1.0f.
  // unfortunately we can only do this if we got the computation
  // in our hands, i.e. for the fast builtin-dt-matrix-profile path.
  for(int k=0; k<3; k++)
  {
    // omit luts marked as linear (negative as marker)
    if(d->lut[k][0] >= 0.0f)
    {
      const float x[4] = {0.7f, 0.8f, 0.9f, 1.0f};
      const float y[4] = {lerp_lut(d->lut[k], x[0]),
                          lerp_lut(d->lut[k], x[1]),
                          lerp_lut(d->lut[k], x[2]),
                          lerp_lut(d->lut[k], x[3])
                         };
      dt_iop_estimate_exp(x, y, 4, d->unbounded_coeffs[k]);
    }
    else d->unbounded_coeffs[k][0] = -1.0f;
  }
}

void init_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  piece->data = malloc(sizeof(dt_iop_colorin_data_t));
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  d->input = NULL;
  d->xform = (cmsHTRANSFORM *)malloc(sizeof(cmsHTRANSFORM)*dt_get_num_threads());
  for(int t=0; t<dt_get_num_threads(); t++) d->xform[t] = NULL;
  d->Lab = dt_colorspaces_create_lab_profile();
  self->commit_params(self, self->default_params, pipe, piece);
}

void cleanup_pipe (struct dt_iop_module_t *self, dt_dev_pixelpipe_t *pipe, dt_dev_pixelpipe_iop_t *piece)
{
  dt_iop_colorin_data_t *d = (dt_iop_colorin_data_t *)piece->data;
  if(d->input) dt_colorspaces_cleanup_profile(d->input);
  dt_colorspaces_cleanup_profile(d->Lab);
  for(int t=0; t<dt_get_num_threads(); t++) if(d->xform[t]) cmsDeleteTransform(d->xform[t]);
  free(d->xform);
  free(piece->data);
}

void gui_update(struct dt_iop_module_t *self)
{
  dt_iop_module_t *module = (dt_iop_module_t *)self;
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  dt_iop_colorin_params_t *p = (dt_iop_colorin_params_t *)module->params;
  // dt_bauhaus_combobox_set(g->cbox1, (int)p->intent);

  update_profile_list(self);

  // TODO: merge this into update_profile_list()
  GList *prof = g->image_profiles;
  while(prof)
  {
    dt_iop_color_profile_t *pp = (dt_iop_color_profile_t *)prof->data;
    if(!strcmp(pp->filename, p->iccprofile))
    {
      dt_bauhaus_combobox_set(g->cbox2, pp->pos);
      return;
    }
    prof = g_list_next(prof);
  }
  prof = g->global_profiles;
  while(prof)
  {
    dt_iop_color_profile_t *pp = (dt_iop_color_profile_t *)prof->data;
    if(!strcmp(pp->filename, p->iccprofile))
    {
      dt_bauhaus_combobox_set(g->cbox2, pp->pos + g->n_image_profiles);
      return;
    }
    prof = g_list_next(prof);
  }
  dt_bauhaus_combobox_set(g->cbox2, 0);
  if(strcmp(p->iccprofile, "darktable")) fprintf(stderr, "[colorin] could not find requested profile `%s'!\n", p->iccprofile);
}

// FIXME: update the gui when we add/remove the eprofile or ematrix
void reload_defaults(dt_iop_module_t *module)
{
  gboolean use_eprofile = FALSE;
  // some file formats like jpeg can have an embedded color profile
  // currently we only support jpeg
  const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, module->dev->image_storage.id);
  if(!cimg->profile)
  {
    char filename[DT_MAX_PATH_LEN];
    gboolean from_cache = TRUE;
    dt_image_full_path(cimg->id, filename, DT_MAX_PATH_LEN, &from_cache);
    const gchar *cc = filename + strlen(filename);
    for(; *cc!='.'&&cc>filename; cc--);
    gchar *ext = g_ascii_strdown(cc+1, -1);
    if(!strcmp(ext, "jpg") || !strcmp(ext, "jpeg"))
    {
      dt_imageio_jpeg_t jpg;
      if(!dt_imageio_jpeg_read_header(filename, &jpg))
      {
        dt_image_t *img = dt_image_cache_write_get(darktable.image_cache, cimg);
        img->profile_size = dt_imageio_jpeg_read_profile(&jpg, &img->profile);
        use_eprofile = (img->profile_size > 0);
        dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
      }
    }
#ifdef HAVE_OPENJPEG
    else if(!strcmp(ext, "jp2") || !strcmp(ext, "j2k") || !strcmp(ext, "j2c") || !strcmp(ext, "jpc"))
    {
      dt_image_t *img = dt_image_cache_write_get(darktable.image_cache, cimg);
      img->profile_size = dt_imageio_j2k_read_profile(filename, &img->profile);
      use_eprofile = (img->profile_size > 0);
      dt_image_cache_write_release(darktable.image_cache, img, DT_IMAGE_CACHE_RELAXED);
    }
#endif
    g_free(ext);
  }
  else
    use_eprofile = TRUE; // the image has a profile assigned
  dt_image_cache_read_release(darktable.image_cache, cimg);

  dt_iop_colorin_params_t tmp = (dt_iop_colorin_params_t)
  {
    "darktable", DT_INTENT_PERCEPTUAL
  };

  if(use_eprofile) g_strlcpy(tmp.iccprofile, "eprofile", sizeof(tmp.iccprofile));
  else if(module->dev->image_storage.colorspace == DT_IMAGE_COLORSPACE_SRGB) g_strlcpy(tmp.iccprofile, "sRGB", sizeof(tmp.iccprofile));
  else if(module->dev->image_storage.colorspace == DT_IMAGE_COLORSPACE_ADOBE_RGB) g_strlcpy(tmp.iccprofile, "adobergb", sizeof(tmp.iccprofile));
  else if(dt_image_is_ldr(&module->dev->image_storage)) g_strlcpy(tmp.iccprofile, "sRGB", sizeof(tmp.iccprofile));
  memcpy(module->params, &tmp, sizeof(dt_iop_colorin_params_t));
  memcpy(module->default_params, &tmp, sizeof(dt_iop_colorin_params_t));
}

void init(dt_iop_module_t *module)
{
  // module->data = malloc(sizeof(dt_iop_colorin_data_t));
  module->params = malloc(sizeof(dt_iop_colorin_params_t));
  module->default_params = malloc(sizeof(dt_iop_colorin_params_t));
  module->params_size = sizeof(dt_iop_colorin_params_t);
  module->gui_data = NULL;
  module->priority = 333; // module order created by iop_dependencies.py, do not edit!
  module->hide_enable_button = 1;
  module->default_enabled = 1;
}

void cleanup(dt_iop_module_t *module)
{
  free(module->gui_data);
  module->gui_data = NULL;
  free(module->params);
  module->params = NULL;
}

static void update_profile_list(dt_iop_module_t *self)
{
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;

  // clear and refill the image profile list
  while(g->image_profiles)
  {
    g_free(g->image_profiles->data);
    g->image_profiles = g_list_delete_link(g->image_profiles, g->image_profiles);
  }
  g->image_profiles = NULL;
  g->n_image_profiles = 0;

  dt_iop_color_profile_t *prof;
  int pos = -1;
  // some file formats like jpeg can have an embedded color profile
  // currently we only support jpeg
  const dt_image_t *cimg = dt_image_cache_read_get(darktable.image_cache, self->dev->image_storage.id);
  if(cimg->profile)
  {
    prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
    g_strlcpy(prof->filename, "eprofile", sizeof(prof->filename));
    g_strlcpy(prof->name, "eprofile", sizeof(prof->name));
    g->image_profiles = g_list_append(g->image_profiles, prof);
    prof->pos = ++pos;
  }
  dt_image_cache_read_release(darktable.image_cache, cimg);
  // use the matrix embedded in some DNGs
  if(!isnan(self->dev->image_storage.d65_color_matrix[0]))
  {
    prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
    g_strlcpy(prof->filename, "ematrix", sizeof(prof->filename));
    g_strlcpy(prof->name, "ematrix", sizeof(prof->name));
    g->image_profiles = g_list_append(g->image_profiles, prof);
    prof->pos = ++pos;
  }
  // get color matrix from raw image:
  char makermodel[1024];
  dt_colorspaces_get_makermodel(makermodel, 1024, self->dev->image_storage.exif_maker, self->dev->image_storage.exif_model);
  float cam_xyz[12];
  cam_xyz[0] = -666.0f;
  dt_dcraw_adobe_coeff(makermodel, "", (float (*)[12])cam_xyz);
  if(cam_xyz[0] != -666.0f)
  {
    prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
    g_strlcpy(prof->filename, "cmatrix", sizeof(prof->filename));
    g_strlcpy(prof->name, "cmatrix", sizeof(prof->name));
    g->image_profiles = g_list_append(g->image_profiles, prof);
    prof->pos = ++pos;
  }

  // darktable built-in, if applicable
  for(int k=0; k<dt_profiled_colormatrix_cnt; k++)
  {
    if(!strcasecmp(makermodel, dt_profiled_colormatrices[k].makermodel))
    {
      prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
      g_strlcpy(prof->filename, "darktable", sizeof(prof->filename));
      g_strlcpy(prof->name, "darktable", sizeof(prof->name));
      g->image_profiles = g_list_append(g->image_profiles, prof);
      prof->pos = ++pos;
      break;
    }
  }

  // darktable vendor matrix, if applicable
  for(int k=0; k<dt_vendor_colormatrix_cnt; k++)
  {
    if(!strcmp(makermodel, dt_vendor_colormatrices[k].makermodel))
    {
      prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
      g_strlcpy(prof->filename, "vendor", sizeof(prof->filename));
      g_strlcpy(prof->name, "vendor", sizeof(prof->name));
      g->image_profiles = g_list_append(g->image_profiles, prof);
      prof->pos = ++pos;
      break;
    }
  }

  // darktable alternate matrix, if applicable
  for(int k=0; k<dt_alternate_colormatrix_cnt; k++)
  {
    if(!strcmp(makermodel, dt_alternate_colormatrices[k].makermodel))
    {
      prof = (dt_iop_color_profile_t *)malloc(sizeof(dt_iop_color_profile_t));
      g_strlcpy(prof->filename, "alternate", sizeof(prof->filename));
      g_strlcpy(prof->name, "alternate", sizeof(prof->name));
      g->image_profiles = g_list_append(g->image_profiles, prof);
      prof->pos = ++pos;
      break;
    }
  }

  g->n_image_profiles = pos + 1;

  // update the gui
  dt_bauhaus_combobox_clear(g->cbox2);

  GList *l = g->image_profiles;
  while(l)
  {
    dt_iop_color_profile_t *prof = (dt_iop_color_profile_t *)l->data;
    if(!strcmp(prof->name, "eprofile"))
      dt_bauhaus_combobox_add(g->cbox2, _("embedded ICC profile"));
    else if(!strcmp(prof->name, "ematrix"))
      dt_bauhaus_combobox_add(g->cbox2, _("DNG embedded matrix"));
    else if(!strcmp(prof->name, "cmatrix"))
      dt_bauhaus_combobox_add(g->cbox2, _("standard color matrix"));
    else if(!strcmp(prof->name, "darktable"))
      dt_bauhaus_combobox_add(g->cbox2, _("enhanced color matrix"));
    else if(!strcmp(prof->name, "vendor"))
      dt_bauhaus_combobox_add(g->cbox2, _("vendor color matrix"));
    else if(!strcmp(prof->name, "alternate"))
      dt_bauhaus_combobox_add(g->cbox2, _("alternate color matrix"));
    else if(!strcmp(prof->name, "sRGB"))
      dt_bauhaus_combobox_add(g->cbox2, _("sRGB (e.g. JPG)"));
    else if(!strcmp(prof->name, "adobergb"))
      dt_bauhaus_combobox_add(g->cbox2, _("Adobe RGB (compatible)"));
    else if(!strcmp(prof->name, "linear_rgb"))
      dt_bauhaus_combobox_add(g->cbox2, _("linear Rec709 RGB"));
    else if(!strcmp(prof->name, "infrared"))
      dt_bauhaus_combobox_add(g->cbox2, _("linear infrared BGR"));
    else if(!strcmp(prof->name, "XYZ"))
      dt_bauhaus_combobox_add(g->cbox2, _("linear XYZ"));
    else if(!strcmp(prof->name, "Lab"))
      dt_bauhaus_combobox_add(g->cbox2, _("Lab"));
    else
      dt_bauhaus_combobox_add(g->cbox2, prof->name);
    l = g_list_next(l);
  }
  l = g->global_profiles;
  while(l)
  {
    dt_iop_color_profile_t *prof = (dt_iop_color_profile_t *)l->data;
    if(!strcmp(prof->name, "eprofile"))
      dt_bauhaus_combobox_add(g->cbox2, _("embedded ICC profile"));
    else if(!strcmp(prof->name, "ematrix"))
      dt_bauhaus_combobox_add(g->cbox2, _("DNG embedded matrix"));
    else if(!strcmp(prof->name, "cmatrix"))
      dt_bauhaus_combobox_add(g->cbox2, _("standard color matrix"));
    else if(!strcmp(prof->name, "darktable"))
      dt_bauhaus_combobox_add(g->cbox2, _("enhanced color matrix"));
    else if(!strcmp(prof->name, "vendor"))
      dt_bauhaus_combobox_add(g->cbox2, _("vendor color matrix"));
    else if(!strcmp(prof->name, "alternate"))
      dt_bauhaus_combobox_add(g->cbox2, _("alternate color matrix"));
    else if(!strcmp(prof->name, "sRGB"))
      dt_bauhaus_combobox_add(g->cbox2, _("sRGB (e.g. JPG)"));
    else if(!strcmp(prof->name, "adobergb"))
      dt_bauhaus_combobox_add(g->cbox2, _("Adobe RGB (compatible)"));
    else if(!strcmp(prof->name, "linear_rgb"))
      dt_bauhaus_combobox_add(g->cbox2, _("linear Rec709 RGB"));
    else if(!strcmp(prof->name, "infrared"))
      dt_bauhaus_combobox_add(g->cbox2, _("linear infrared BGR"));
    else if(!strcmp(prof->name, "XYZ"))
      dt_bauhaus_combobox_add(g->cbox2, _("linear XYZ"));
    else if(!strcmp(prof->name, "Lab"))
      dt_bauhaus_combobox_add(g->cbox2, _("Lab"));
    else
      dt_bauhaus_combobox_add(g->cbox2, prof->name);
    l = g_list_next(l);
  }
}

void gui_init(struct dt_iop_module_t *self)
{
  // pthread_mutex_lock(&darktable.plugin_threadsafe);
  self->gui_data = malloc(sizeof(dt_iop_colorin_gui_data_t));
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;

  g->image_profiles = g->global_profiles = NULL;
  dt_iop_color_profile_t *prof;

  // the profiles that are available for every image
  int pos = -1;

  // add linear Rec709 RGB profile:
  prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "linear_rgb", sizeof(prof->filename));
  g_strlcpy(prof->name, "linear_rgb", sizeof(prof->name));
  g->global_profiles = g_list_append(g->global_profiles, prof);
  prof->pos = ++pos;

  // sRGB for ldr image input
  prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "sRGB", sizeof(prof->filename));
  g_strlcpy(prof->name, "sRGB", sizeof(prof->name));
  g->global_profiles = g_list_append(g->global_profiles, prof);
  prof->pos = ++pos;

  // adobe rgb built-in
  prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "adobergb", sizeof(prof->filename));
  g_strlcpy(prof->name, "adobergb", sizeof(prof->name));
  g->global_profiles = g_list_append(g->global_profiles, prof);
  prof->pos = ++pos;

  // XYZ built-in
  prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "XYZ", sizeof(prof->filename));
  g_strlcpy(prof->name, "XYZ", sizeof(prof->name));
  g->global_profiles = g_list_append(g->global_profiles, prof);
  prof->pos = ++pos;

  // Lab built-in
  prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "Lab", sizeof(prof->filename));
  g_strlcpy(prof->name, "Lab", sizeof(prof->name));
  g->global_profiles = g_list_append(g->global_profiles, prof);
  prof->pos = ++pos;

  // infrared built-in
  prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
  g_strlcpy(prof->filename, "infrared", sizeof(prof->filename));
  g_strlcpy(prof->name, "infrared", sizeof(prof->name));
  g->global_profiles = g_list_append(g->global_profiles, prof);
  prof->pos = ++pos;

  // read {userconfig,datadir}/color/in/*.icc, in this order.
  char datadir[DT_MAX_PATH_LEN];
  char confdir[DT_MAX_PATH_LEN];
  char dirname[DT_MAX_PATH_LEN];
  char filename[DT_MAX_PATH_LEN];
  dt_loc_get_user_config_dir(confdir, DT_MAX_PATH_LEN);
  dt_loc_get_datadir(datadir, DT_MAX_PATH_LEN);
  snprintf(dirname, DT_MAX_PATH_LEN, "%s/color/in", confdir);
  if(!g_file_test(dirname, G_FILE_TEST_IS_DIR))
    snprintf(dirname, DT_MAX_PATH_LEN, "%s/color/in", datadir);
  cmsHPROFILE tmpprof;
  const gchar *d_name;
  GDir *dir = g_dir_open(dirname, 0, NULL);
  if(dir)
  {
    while((d_name = g_dir_read_name(dir)))
    {
      if(!strcmp(d_name, "linear_rgb")) continue;
      snprintf(filename, DT_MAX_PATH_LEN, "%s/%s", dirname, d_name);
      tmpprof = cmsOpenProfileFromFile(filename, "r");
      if(tmpprof)
      {
        char *lang = getenv("LANG");
        if (!lang) lang = "en_US";

        dt_iop_color_profile_t *prof = (dt_iop_color_profile_t *)g_malloc0(sizeof(dt_iop_color_profile_t));
        char name[1024];
        cmsGetProfileInfoASCII(tmpprof, cmsInfoDescription, lang, lang+3, name, 1024);
        g_strlcpy(prof->name, name, sizeof(prof->name));

        g_strlcpy(prof->filename, d_name, sizeof(prof->filename));
        cmsCloseProfile(tmpprof);
        g->global_profiles = g_list_append(g->global_profiles, prof);
        prof->pos = ++pos;
      }
    }
    g_dir_close(dir);
  }

  self->widget = gtk_vbox_new(TRUE, DT_BAUHAUS_SPACE);
  g->cbox2 = dt_bauhaus_combobox_new(self);
  dt_bauhaus_widget_set_label(g->cbox2, NULL, _("profile"));
  gtk_box_pack_start(GTK_BOX(self->widget), g->cbox2, TRUE, TRUE, 0);

  // now generate the list of profiles applicable to the current image and update the list
  update_profile_list(self);

  dt_bauhaus_combobox_set(g->cbox2, 0);

  char tooltip[1024];
  snprintf(tooltip, 1024, _("ICC profiles in %s/color/in or %s/color/in"), confdir, datadir);
  g_object_set(G_OBJECT(g->cbox2), "tooltip-text", tooltip, (char *)NULL);

  g_signal_connect (G_OBJECT (g->cbox2), "value-changed",
                    G_CALLBACK (profile_changed),
                    (gpointer)self);
}

void gui_cleanup(struct dt_iop_module_t *self)
{
  dt_iop_colorin_gui_data_t *g = (dt_iop_colorin_gui_data_t *)self->gui_data;
  while(g->image_profiles)
  {
    g_free(g->image_profiles->data);
    g->image_profiles = g_list_delete_link(g->image_profiles, g->image_profiles);
  }
  while(g->global_profiles)
  {
    g_free(g->global_profiles->data);
    g->global_profiles = g_list_delete_link(g->global_profiles, g->global_profiles);
  }
  free(self->gui_data);
  self->gui_data = NULL;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
