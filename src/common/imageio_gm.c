/*
    This file is part of darktable,
    copyright (c) 2012 Ulrich Pegelow.

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

#ifdef HAVE_GRAPHICSMAGICK
#include "common/darktable.h"
#include "imageio.h"
#include "imageio_gm.h"
#include "develop/develop.h"
#include "common/exif.h"
#include "common/colorspaces.h"
#include "control/conf.h"

#include <memory.h>
#include <stdio.h>
#include <inttypes.h>
#include <strings.h>
#include <magick/api.h>
#include <assert.h>

dt_imageio_retval_t
dt_imageio_open_gm(
  dt_image_t *img,
  const char *filename,
  dt_mipmap_cache_allocator_t a)
{
  int err = DT_IMAGEIO_FILE_CORRUPTED;
  float *buf = NULL;
  ExceptionInfo exception;
  Image *image = NULL;
  ImageInfo *image_info = NULL;
  uint32_t width, height, orientation;


  if(!img->exif_inited)
    (void) dt_exif_read(img, filename);

  GetExceptionInfo(&exception);
  image_info=CloneImageInfo((ImageInfo *) NULL);

  g_strlcpy(image_info->filename,filename,sizeof(image_info->filename));

  image=ReadImage(image_info,&exception);
  if (exception.severity != UndefinedException)
    CatchException(&exception);
  if (!image)
  {
    fprintf(stderr, "[GraphicsMagick_open] image `%s' not found\n", img->filename);
    err = DT_IMAGEIO_FILE_NOT_FOUND;
    goto error;
  }

  width = image->columns;
  height = image->rows;
  orientation = image->orientation;

  if(orientation & 4)
  {
    img->width = height;
    img->height = width;
  }
  else
  {
    img->width = width;
    img->height = height;
  }

  img->bpp = 4*sizeof(float);

  float *mipbuf = (float *)dt_mipmap_cache_alloc(img, DT_MIPMAP_FULL, a);
  if(!mipbuf)
  {
    fprintf(stderr, "[GraphicsMagick_open] could not alloc full buffer for image `%s'\n", img->filename);
    err = DT_IMAGEIO_CACHE_FULL;
    goto error;
  }

  buf = (float *)dt_alloc_align(16, width*img->bpp);
  if(!buf) goto error;

  const int ht2 = orientation & 4 ? img->width  : img->height; // pretend unrotated, rotate in write_pos
  const int wd2 = orientation & 4 ? img->height : img->width;

  for (uint32_t row = 0; row < height; row++)
  {
    int ret = DispatchImage(image, 0, row, width, 1, "RGBP", FloatPixel, (void *)buf, &exception);
    if (exception.severity != UndefinedException)
      CatchException(&exception);
    if(ret != MagickPass)
    {
      fprintf(stderr, "[GraphicsMagick_open] error reading image `%s'\n", img->filename);
      err = DT_IMAGEIO_FILE_CORRUPTED;
      goto error;
    }

    for(uint32_t i=0; i<width; i++)
      for(int k=0; k<4; k++) mipbuf[4*dt_imageio_write_pos(i, row, wd2, ht2, wd2, ht2, orientation) + k] = buf[4*i + k];
  }

  if(buf) free(buf);
  if(image) DestroyImage(image);
  if(image_info) DestroyImageInfo(image_info);
  DestroyExceptionInfo(&exception);

  img->filters = 0;
  img->flags &= ~DT_IMAGE_RAW;
  img->flags &= ~DT_IMAGE_HDR;
  img->flags |= DT_IMAGE_LDR;

  return DT_IMAGEIO_OK;

error:
  if(buf) free(buf);
  if(image) DestroyImage(image);
  if(image_info) DestroyImageInfo(image_info);
  DestroyExceptionInfo(&exception);
  return err;
}
#endif

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
