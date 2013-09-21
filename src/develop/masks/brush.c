/*
    This file is part of darktable,
    copyright (c) 2011 henrik andersson.
    copyright (c) 2012 aldric renaudin.
    copyright (c) 2013 Ulrich Pegelow.

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
#include "develop/imageop.h"
#include "develop/blend.h"
#include "control/control.h"
#include "control/conf.h"
#include "develop/masks.h"
#include "common/debug.h"

/** a poor man's memory management: just a sloppy monitoring of buffer usage with automatic reallocation */
static int _brush_buffer_grow(float **buffer, int *buffer_count, int *buffer_max)
{
  const int stepsize = 200000;
  const int reserve = 20000;

  //printf("buffer %p, buffer_count %d, buffer_max %d\n", *buffer, *buffer_count, *buffer_max);

  if(*buffer == NULL)
  {
    *buffer = malloc(stepsize*sizeof(float));
    *buffer_count = 0;
    *buffer_max = stepsize;
    return TRUE;
  }

  if(*buffer_count > *buffer_max)
  {
    fprintf(stderr, "_brush_buffer_grow: memory size exceeded and detected too late :(\n");
  }

  if(*buffer_count + reserve > *buffer_max)
  {
    float *oldbuffer = *buffer;
    *buffer_max += stepsize;
    *buffer = malloc(*buffer_max*sizeof(float));
    if(*buffer == NULL) return FALSE;
    memset(*buffer, 0, *buffer_max*sizeof(float));
    memcpy(*buffer, oldbuffer, *buffer_count*sizeof(float));
    free(oldbuffer);
  }

  return TRUE;
}


/** get squared distance of point to line segment */
static float _brush_point_line_distance2(const float x, const float y, const float *line_start, const float *line_end)
{
  const float r1 = x - line_start[0];
  const float r2 = y - line_start[1];
  const float r3 = line_end[0] - line_start[0];
  const float r4 = line_end[1] - line_start[1];

  const float d = r1*r3 + r2*r4;
  const float l = r3*r3 + r4*r4;
  const float p = d / l;

  float xx, yy;

  if (l == 0.0f)
  {
    xx = line_start[0];
    yy = line_start[1];
  }
  else
  {
    xx = line_start[0] + p * r3;
    yy = line_start[1] + p * r4;
  }

  const float dx = x - xx;
  const float dy = y - yy;

  return dx*dx + dy*dy;
}

/** remove unneeded points (Ramer-Douglas-Peucker algorithm) and return resulting path as linked list */
static GList *_brush_ramer_douglas_peucker(const float *points, int points_count, const float *payload, float epsilon2, dt_masks_pressure_sensitivity_t psens)
{
  GList *ResultList = NULL;

  float dmax2 = 0.0f;
  int index = 0;

  for (int i = 1; i < points_count-1; i++)
  {
    float d2 = _brush_point_line_distance2(points[i*2], points[i*2+1], points, points+points_count-1);
    if (d2 > dmax2)
    {
      index = i;
      dmax2 = d2;
    }
  }

  if (dmax2 >= epsilon2)
  {
    GList *ResultList1 = _brush_ramer_douglas_peucker(points, index+1, payload, epsilon2, psens);
    GList *ResultList2 = _brush_ramer_douglas_peucker(points+index*2, points_count-index, payload+index*4, epsilon2, psens);

    // remove last element from ResultList1
    GList *end1 = g_list_last(ResultList1);
    free(end1->data);
    ResultList1 = g_list_delete_link(ResultList1, end1);

    ResultList = g_list_concat(ResultList1, ResultList2);
  }
  else
  {
    dt_masks_point_brush_t *point1 = malloc(sizeof(dt_masks_point_brush_t));
    point1->corner[0] = points[0];
    point1->corner[1] = points[1];
    point1->ctrl1[0] = point1->ctrl1[1] = point1->ctrl2[0] = point1->ctrl2[1] = -1.0f;
    point1->border[0] = point1->border[1] = payload[0];
    point1->hardness = payload[1];
    point1->density = payload[2];
    point1->state = DT_MASKS_POINT_STATE_NORMAL;
    float pressure = payload[3];
    switch(psens)
    {
      case DT_MASKS_PRESSURE_HARDNESS_ABS:
        point1->hardness = MAX(0.05f, pressure);
        break;
      case DT_MASKS_PRESSURE_HARDNESS_REL:
        point1->hardness = MAX(0.05f, point1->hardness*pressure);
        break;
      case DT_MASKS_PRESSURE_OPACITY_ABS:
        point1->density = MAX(0.05f, pressure);
        break;
      case DT_MASKS_PRESSURE_OPACITY_REL:
        point1->density = MAX(0.05f, point1->density*pressure);
        break;
      case DT_MASKS_PRESSURE_BRUSHSIZE_REL:
        point1->border[0] = MAX(0.005f, point1->border[0]*pressure);
        point1->border[1] = MAX(0.005f, point1->border[1]*pressure);
        break;
      default:
      case DT_MASKS_PRESSURE_OFF:
        //ignore pressure value
        break;
    }
    ResultList = g_list_append(ResultList, (gpointer)point1);

    dt_masks_point_brush_t *pointn = malloc(sizeof(dt_masks_point_brush_t));
    pointn->corner[0] = points[(points_count-1)*2];
    pointn->corner[1] = points[(points_count-1)*2+1];
    pointn->ctrl1[0] = pointn->ctrl1[1] = pointn->ctrl2[0] = pointn->ctrl2[1] = -1.0f;
    pointn->border[0] = pointn->border[1] = payload[(points_count-1)*4];
    pointn->hardness = payload[(points_count-1)*4+1];
    pointn->density = payload[(points_count-1)*4+2];
    pointn->state = DT_MASKS_POINT_STATE_NORMAL;
    pressure = payload[(points_count-1)*4+3];
    switch(psens)
    {
      case DT_MASKS_PRESSURE_HARDNESS_ABS:
        pointn->hardness = MAX(0.05f, pressure);
        break;
      case DT_MASKS_PRESSURE_HARDNESS_REL:
        pointn->hardness = MAX(0.05f, pointn->hardness*pressure);
        break;
      case DT_MASKS_PRESSURE_OPACITY_ABS:
        pointn->density = MAX(0.05f, pressure);
        break;
      case DT_MASKS_PRESSURE_OPACITY_REL:
        pointn->density = MAX(0.05f, pointn->density*pressure);
        break;
      case DT_MASKS_PRESSURE_BRUSHSIZE_REL:
        pointn->border[0] = MAX(0.005f, pointn->border[0]*pressure);
        pointn->border[1] = MAX(0.005f, pointn->border[1]*pressure);
        break;
      default:
      case DT_MASKS_PRESSURE_OFF:
        //ignore pressure value
        break;
    }
    ResultList = g_list_append(ResultList, (gpointer)pointn);
  }

  return ResultList;
}

/** get the point of the brush at pos t [0,1]  */
static void _brush_get_XY(float p0x, float p0y, float p1x, float p1y, float p2x, float p2y, float p3x, float p3y,
                          float t, float *x, float *y)
{
  float a = (1-t)*(1-t)*(1-t);
  float b = 3*t*(1-t)*(1-t);
  float c = 3*t*t*(1-t);
  float d = t*t*t;
  *x =  p0x*a + p1x*b + p2x*c + p3x*d;
  *y =  p0y*a + p1y*b + p2y*c + p3y*d;
}

/** get the point of the brush at pos t [0,1]  AND the corresponding border point */
static void _brush_border_get_XY(float p0x, float p0y, float p1x, float p1y, float p2x, float p2y, float p3x, float p3y,
                                 float t, float rad, float *xc, float *yc, float *xb, float *yb)
{
  //we get the point
  _brush_get_XY(p0x,p0y,p1x,p1y,p2x,p2y,p3x,p3y,t,xc,yc);

  //now we get derivative points
  float a = 3*(1-t)*(1-t);
  float b = 3*((1-t)*(1-t) - 2*t*(1-t));
  float c = 3*(2*t*(1-t)-t*t);
  float d = 3*t*t;

  float dx = -p0x*a + p1x*b + p2x*c + p3x*d;
  float dy = -p0y*a + p1y*b + p2y*c + p3y*d;

  //so we can have the resulting point
  if (dx==0 && dy==0)
  {
    *xb = -9999999;
    *yb = -9999999;
    return;
  }
  float l = 1.0/sqrtf(dx*dx+dy*dy);
  *xb = (*xc) + rad*dy*l;
  *yb = (*yc) - rad*dx*l;
}

/** get feather extremity from the control point n°2 */
/** the values should be in orthonormal space */
static void _brush_ctrl2_to_feather(int ptx,int pty, int ctrlx, int ctrly, int *fx, int *fy, gboolean clockwise)
{
  if (clockwise)
  {
    *fx = ptx + ctrly - pty;
    *fy = pty + ptx - ctrlx;
  }
  else
  {
    *fx = ptx - ctrly + pty;
    *fy = pty - ptx + ctrlx;
  }
}

/** get bezier control points from feather extremity */
/** the values should be in orthonormal space */
static void _brush_feather_to_ctrl(int ptx,int pty, int fx, int fy, int *ctrl1x, int *ctrl1y, int *ctrl2x, int *ctrl2y, gboolean clockwise)
{
  if (clockwise)
  {
    *ctrl2x = ptx + pty - fy;
    *ctrl2y = pty + fx - ptx;
    *ctrl1x = ptx - pty + fy;
    *ctrl1y = pty - fx + ptx;
  }
  else
  {
    *ctrl1x = ptx + pty - fy;
    *ctrl1y = pty + fx - ptx;
    *ctrl2x = ptx - pty + fy;
    *ctrl2y = pty - fx + ptx;
  }
}

/** Get the control points of a segment to match exactly a catmull-rom spline */
static void _brush_catmull_to_bezier(float x1, float y1, float x2, float y2, float x3, float y3, float x4, float y4,
                                     float* bx1, float* by1, float* bx2, float* by2)
{
  *bx1 = (-x1 + 6*x2 + x3) / 6;
  *by1 = (-y1 + 6*y2 + y3) / 6;
  *bx2 = ( x2 + 6*x3 - x4) / 6;
  *by2 = ( y2 + 6*y3 - y4) / 6;
}

/** initialise all control points to eventually match a catmull-rom like spline */
static void _brush_init_ctrl_points (dt_masks_form_t *form)
{
  //if we have less that 2 points, what to do ??
  if (g_list_length(form->points) < 2) return;

  //we need extra points to deal with curve ends
  dt_masks_point_brush_t start_point[2], end_point[2];

  int nb = g_list_length(form->points);
  for(int k = 0; k < nb; k++)
  {
    dt_masks_point_brush_t *point3 = (dt_masks_point_brush_t *)g_list_nth_data(form->points,k);
    //if the point as not be set manually, we redfine it
    if (point3->state & DT_MASKS_POINT_STATE_NORMAL)
    {
      //we want to get point-2, point-1, point+1, point+2
      dt_masks_point_brush_t *point1 = k-2 >= 0 ? (dt_masks_point_brush_t *)g_list_nth_data(form->points,k-2) : NULL;
      dt_masks_point_brush_t *point2 = k-1 >= 0 ? (dt_masks_point_brush_t *)g_list_nth_data(form->points,k-1) : NULL;
      dt_masks_point_brush_t *point4 = k+1 < nb ? (dt_masks_point_brush_t *)g_list_nth_data(form->points,k+1) : NULL;
      dt_masks_point_brush_t *point5 = k+2 < nb ? (dt_masks_point_brush_t *)g_list_nth_data(form->points,k+2) : NULL;

      //deal with end points: make both extending points mirror their neighborhood
      if (point1 == NULL && point2 == NULL)
      {
        start_point[0].corner[0] = start_point[1].corner[0] = 2*point3->corner[0] - point4->corner[0];
        start_point[0].corner[1] = start_point[1].corner[1] = 2*point3->corner[1] - point4->corner[1];
        point1 = &(start_point[0]);
        point2 = &(start_point[1]);
      }
      else if (point1 == NULL)
      {
        start_point[0].corner[0] = 2*point2->corner[0] - point3->corner[0];
        start_point[0].corner[1] = 2*point2->corner[1] - point3->corner[1];
        point1 = &(start_point[0]);
      }

      if (point4 == NULL && point5 == NULL)
      {
        end_point[0].corner[0] = end_point[1].corner[0] = 2*point3->corner[0] - point2->corner[0];
        end_point[0].corner[1] = end_point[1].corner[1] = 2*point3->corner[1] - point2->corner[1];
        point4 = &(end_point[0]);
        point5 = &(end_point[1]);
      }
      else if (point5 == NULL)
      {
        end_point[0].corner[0] = 2*point4->corner[0] - point3->corner[0];
        end_point[0].corner[1] = 2*point4->corner[1] - point3->corner[1];
        point5 = &(end_point[0]);
      }


      float bx1,by1,bx2,by2;
      _brush_catmull_to_bezier(point1->corner[0],point1->corner[1],
                               point2->corner[0],point2->corner[1],
                               point3->corner[0],point3->corner[1],
                               point4->corner[0],point4->corner[1],
                               &bx1,&by1,&bx2,&by2);
      if (point2->ctrl2[0] == -1.0) point2->ctrl2[0] = bx1;
      if (point2->ctrl2[1] == -1.0) point2->ctrl2[1] = by1;
      point3->ctrl1[0] = bx2;
      point3->ctrl1[1] = by2;
      _brush_catmull_to_bezier(point2->corner[0],point2->corner[1],
                               point3->corner[0],point3->corner[1],
                               point4->corner[0],point4->corner[1],
                               point5->corner[0],point5->corner[1],
                               &bx1,&by1,&bx2,&by2);
      if (point4->ctrl1[0] == -1.0) point4->ctrl1[0] = bx2;
      if (point4->ctrl1[1] == -1.0) point4->ctrl1[1] = by2;
      point3->ctrl2[0] = bx1;
      point3->ctrl2[1] = by1;
    }
  }
}


/** fill the gap between 2 points with an arc of circle */
/** this function is here because we can have gap in border, esp. if the corner is very sharp */
static void _brush_points_recurs_border_gaps(float *cmax, float *bmin, float *bmin2, float *bmax, float *points, int *pos_points, float *border, int *pos_border, gboolean clockwise)
{
  //we want to find the start and end angles
  float a1 = atan2(bmin[1]-cmax[1],bmin[0]-cmax[0]);
  float a2 = atan2(bmax[1]-cmax[1],bmax[0]-cmax[0]);

  if (a1 == a2) return;

  //we have to be sure that we turn in the correct direction
  if (a2<a1 && clockwise)
  {
    a2 += 2.0f*M_PI;
  }
  if (a2>a1 && !clockwise)
  {
    a1 += 2.0f*M_PI;
  }

  //we determine start and end radius too
  float r1 = sqrtf((bmin[1]-cmax[1])*(bmin[1]-cmax[1])+(bmin[0]-cmax[0])*(bmin[0]-cmax[0]));
  float r2 = sqrtf((bmax[1]-cmax[1])*(bmax[1]-cmax[1])+(bmax[0]-cmax[0])*(bmax[0]-cmax[0]));

  //and the max length of the circle arc
  int l;
  if (a2>a1) l = (a2-a1)*fmaxf(r1,r2);
  else l = (a1-a2)*fmaxf(r1,r2);
  if (l<2) return;

  //and now we add the points
  float incra = (a2-a1)/l;
  float incrr = (r2-r1)/l;
  float rr = r1+incrr;
  float aa = a1+incra;
  for (int i=1; i<l; i++)
  {
    points[*pos_points] = cmax[0];
    points[*pos_points+1] = cmax[1];
    *pos_points += 2;
    border[*pos_border] = cmax[0]+rr*cosf(aa);
    border[*pos_border+1] = cmax[1]+rr*sinf(aa);
    *pos_border += 2;
    rr += incrr;
    aa += incra;
  }
}

/** fill small gap between 2 points with an arc of circle */
/** in contrast to the previous function it will always run the shortest path (max. PI) and does not consider clock or anti-clockwise action */
static void _brush_points_recurs_border_small_gaps(float *cmax, float *bmin, float *bmin2, float *bmax, float *points, int *pos_points, float *border, int *pos_border)
{
  //we want to find the start and end angles
  float a1 = fmodf(atan2(bmin[1]-cmax[1],bmin[0]-cmax[0]) + 2.0f*M_PI, 2.0f*M_PI);
  float a2 = fmodf(atan2(bmax[1]-cmax[1],bmax[0]-cmax[0]) + 2.0f*M_PI, 2.0f*M_PI);

  if (a1 == a2) return;

  //we determine start and end radius too
  float r1 = sqrtf((bmin[1]-cmax[1])*(bmin[1]-cmax[1])+(bmin[0]-cmax[0])*(bmin[0]-cmax[0]));
  float r2 = sqrtf((bmax[1]-cmax[1])*(bmax[1]-cmax[1])+(bmax[0]-cmax[0])*(bmax[0]-cmax[0]));

  //and the max length of the circle arc
  int l = fmodf(fabsf(a2-a1), M_PI)*fmaxf(r1,r2);
  if (l < 2) return;

  //and now we add the points
  float incra = (a2-a1)/l;
  float incrr = (r2-r1)/l;
  float rr = r1+incrr;
  float aa = a1+incra;
  for (int i=1; i<l; i++)
  {
    points[*pos_points] = cmax[0];
    points[*pos_points+1] = cmax[1];
    *pos_points += 2;
    border[*pos_border] = cmax[0]+rr*cosf(aa);
    border[*pos_border+1] = cmax[1]+rr*sinf(aa);
    *pos_border += 2;
    rr += incrr;
    aa += incra;
  }
}


/** draw a circle with given radius. can be used to terminate a stroke and to draw junctions where attributes (opacity) change */
static void _brush_points_stamp(float *cmax, float *bmin, float *points, int *pos_points, float *border, int *pos_border, gboolean clockwise)
{
  //we want to find the start angle
  float a1 = atan2(bmin[1]-cmax[1],bmin[0]-cmax[0]);

  //we determine the radius too
  float rad = sqrtf((bmin[1]-cmax[1])*(bmin[1]-cmax[1])+(bmin[0]-cmax[0])*(bmin[0]-cmax[0]));

  //determine the max length of the circle arc
  int l = 2.0f*M_PI*rad;
  if (l<2) return;

  //and now we add the points
  float incra = 2.0f*M_PI/l;
  float aa = a1+incra;
  for (int i=0; i<l; i++)
  {
    points[*pos_points] = cmax[0];
    points[*pos_points+1] = cmax[1];
    *pos_points += 2;
    border[*pos_border] = cmax[0]+rad*cosf(aa);
    border[*pos_border+1] = cmax[1]+rad*sinf(aa);
    *pos_border += 2;
    aa += incra;
  }
}

/** recursive function to get all points of the brush AND all point of the border */
/** the function takes care to avoid big gaps between points */
static void _brush_points_recurs(float *p1, float *p2,
                                 double tmin, double tmax, float *points_min, float *points_max, float *border_min, float *border_max,
                                 float *rpoints, float *rborder, float *rpayload, float *points, float *border, float *payload, 
                                 int *pos_points, int *pos_border, int *pos_payload, int withborder, int withpayload)
{
  //we calculate points if needed
  if (points_min[0] == -99999)
  {
    _brush_border_get_XY(p1[0],p1[1],p1[2],p1[3],p2[2],p2[3],p2[0],p2[1],tmin, p1[4]+(p2[4]-p1[4])*tmin*tmin*(3.0-2.0*tmin),
                         points_min,points_min+1,border_min,border_min+1);
  }
  if (points_max[0] == -99999)
  {
    _brush_border_get_XY(p1[0],p1[1],p1[2],p1[3],p2[2],p2[3],p2[0],p2[1],tmax, p1[4]+(p2[4]-p1[4])*tmax*tmax*(3.0-2.0*tmax),
                         points_max,points_max+1,border_max,border_max+1);
  }
  //are the points near ?
  if ((tmax-tmin < 0.0001f) || ((int)points_min[0]-(int)points_max[0]<2 && (int)points_min[0]-(int)points_max[0]>-2 &&
                               (int)points_min[1]-(int)points_max[1]<2 && (int)points_min[1]-(int)points_max[1]>-2 &&
                               (!withborder || (
                                  (int)border_min[0]-(int)border_max[0]<2 && (int)border_min[0]-(int)border_max[0]>-2 &&
                                  (int)border_min[1]-(int)border_max[1]<2 && (int)border_min[1]-(int)border_max[1]>-2))))
  {
    points[*pos_points] = points_max[0];
    points[*pos_points+1] = points_max[1];
    *pos_points += 2;
    rpoints[0] = points_max[0];
    rpoints[1] = points_max[1];

    if (withborder)
    {
      if (border_max[0] == -9999999.0f)
      {
        border_max[0] = border_min[0];
        border_max[1] = border_min[1];
      }

      //we check gaps in the border (sharp edges)
      if (labs((int)border_max[0] - (int)border_min[0]) > 2 || labs((int)border_max[1] - border_min[1]) > 2)
      {
        _brush_points_recurs_border_small_gaps(points_max, border_min, NULL, border_max, points, pos_points, border, pos_border);
      }

      rborder[0] = border[*pos_border] = border_max[0];
      rborder[1] = border[*pos_border+1] = border_max[1];
      *pos_border += 2;
    }

    if (withpayload)
    {
      rpayload[0] = payload[*pos_payload] = p1[5] + tmax * (p2[5] - p1[5]);
      rpayload[1] = payload[*pos_payload+1] = p1[6] + tmax * (p2[6] - p1[6]);
      *pos_payload += 2;
    }

    return;
  }

  //we split in two part
  double tx = (tmin+tmax)/2.0;
  float c[2] = {-99999,-99999}, b[2]= {-99999,-99999};
  float rc[2], rb[2], rp[2];
  _brush_points_recurs(p1,p2,tmin,tx,points_min,c,border_min,b,rc,rb,rp,points,border,payload,pos_points,pos_border,pos_payload,withborder,withpayload);
  _brush_points_recurs(p1,p2,tx,tmax,rc,points_max,rb,border_max,rpoints,rborder,rpayload,points,border,payload,pos_points,pos_border,pos_payload,withborder,withpayload);
}


/** converts n into a cyclical sequence counting upwards from 0 to nb-1 and back down again, counting endpoints twice */
static inline int _brush_cyclic_cursor(int n, int nb)
{
  const int o = n % (2*nb);
  const int p = o % nb;

  return (o <= p) ? o : o-2*p-1;
}


/** get all points of the brush and the border */
/** this takes care of gaps and iop distortions */
static int _brush_get_points_border(dt_develop_t *dev, dt_masks_form_t *form, int prio_max, dt_dev_pixelpipe_t *pipe,
                                    float **points, int *points_count, float **border, int *border_count, float **payload, int *payload_count, int source)
{
  double start2 = dt_get_wtime();

  float wd = pipe->iwidth, ht = pipe->iheight;

  int points_max, pos;
  int border_max, posb;
  int payload_max, posp;

  *points = NULL;
  points_max = 0;
  pos = 0;
  if (!_brush_buffer_grow(points, &pos, &points_max)) return 0;

  if (border)
  {
    *border = NULL;
    border_max = 0;
    posb = 0;
    if (!_brush_buffer_grow(border, &posb, &border_max)) return 0;
  }

  if (payload)
  {
    *payload = NULL;
    payload_max = 0;
    posp = 0;
    if (!_brush_buffer_grow(payload, &posp, &payload_max)) return 0;
  }

  //we store all points
  float dx = 0.0f, dy = 0.0f;

  int nb = g_list_length(form->points);

  if (source && nb>0)
  {
    dt_masks_point_brush_t *pt = (dt_masks_point_brush_t *)g_list_nth_data(form->points,0);
    dx = (pt->corner[0]-form->source[0])*wd;
    dy = (pt->corner[1]-form->source[1])*ht;
  }

  for(int k = 0; k < nb; k++)
  {
    dt_masks_point_brush_t *pt = (dt_masks_point_brush_t *)g_list_nth_data(form->points,k);
    (*points)[k*6] = pt->ctrl1[0]*wd-dx;
    (*points)[k*6+1] = pt->ctrl1[1]*ht-dy;
    (*points)[k*6+2] = pt->corner[0]*wd-dx;
    (*points)[k*6+3] = pt->corner[1]*ht-dy;
    (*points)[k*6+4] = pt->ctrl2[0]*wd-dx;
    (*points)[k*6+5] = pt->ctrl2[1]*ht-dy;
  }

  //for the border, we store value too
  if (border)
  {
    for(int k = 0; k < nb; k++)
    {
      (*border)[k*6] = 0.0; //x position of the border point
      (*border)[k*6+1] = 0.0; //y position of the border point
      (*border)[k*6+2] = 0.0; //start index for the initial gap. if <0 this mean we have to skip to index (-x)
      (*border)[k*6+3] = 0.0; //end index for the initial gap
      (*border)[k*6+4] = 0.0; //start index for the final gap. if <0 this mean we have to stop at index (-x)
      (*border)[k*6+5] = 0.0; //end index for the final gap
    }
  }

  //for the payload, we reserve an equivalent number of cells to keep it in sync 
  if (payload)
  {
    for(int k = 0; k < nb; k++)
    {
      (*payload)[k*6] = 0.0;
      (*payload)[k*6+1] = 0.0;
      (*payload)[k*6+2] = 0.0;
      (*payload)[k*6+3] = 0.0;
      (*payload)[k*6+4] = 0.0;
      (*payload)[k*6+5] = 0.0;
    }
  }

  pos = 6*nb;
  posb = 6*nb;
  posp = 6*nb;

  int cw = 1;
  int start_stamp = 0;

  if (darktable.unmuted & DT_DEBUG_PERF) dt_print(DT_DEBUG_MASKS, "[masks %s] brush_points init took %0.04f sec\n", form->name, dt_get_wtime()-start2);
  start2 = dt_get_wtime();

  //we render all segments first upwards, then downwards
  for(int n = 0; n < 2*nb; n++)
  {
    float p1[7], p2[7], p3[7], p4[7];
    int k = _brush_cyclic_cursor(n, nb);
    int k1 = _brush_cyclic_cursor(n+1, nb);
    int k2 = _brush_cyclic_cursor(n+2, nb);

    dt_masks_point_brush_t *point1 = (dt_masks_point_brush_t *)g_list_nth_data(form->points, k);
    dt_masks_point_brush_t *point2 = (dt_masks_point_brush_t *)g_list_nth_data(form->points, k1);
    dt_masks_point_brush_t *point3 = (dt_masks_point_brush_t *)g_list_nth_data(form->points, k2);
    if (cw > 0)
    {    
      float pa[7] = {point1->corner[0]*wd-dx, point1->corner[1]*ht-dy, point1->ctrl2[0]*wd-dx, point1->ctrl2[1]*ht-dy, point1->border[1]*MIN(wd,ht), point1->hardness, point1->density};
      float pb[7] = {point2->corner[0]*wd-dx, point2->corner[1]*ht-dy, point2->ctrl1[0]*wd-dx, point2->ctrl1[1]*ht-dy, point2->border[0]*MIN(wd,ht), point2->hardness, point2->density};
      float pc[7] = {point2->corner[0]*wd-dx, point2->corner[1]*ht-dy, point2->ctrl2[0]*wd-dx, point2->ctrl2[1]*ht-dy, point2->border[1]*MIN(wd,ht), point2->hardness, point2->density};
      float pd[7] = {point3->corner[0]*wd-dx, point3->corner[1]*ht-dy, point3->ctrl1[0]*wd-dx, point3->ctrl1[1]*ht-dy, point3->border[0]*MIN(wd,ht), point3->hardness, point3->density};
      memcpy(p1, pa, 7*sizeof(float));
      memcpy(p2, pb, 7*sizeof(float));
      memcpy(p3, pc, 7*sizeof(float));
      memcpy(p4, pd, 7*sizeof(float));
    }
    else
    {
      float pa[7] = {point1->corner[0]*wd-dx, point1->corner[1]*ht-dy, point1->ctrl1[0]*wd-dx, point1->ctrl1[1]*ht-dy, point1->border[1]*MIN(wd,ht), point1->hardness, point1->density};
      float pb[7] = {point2->corner[0]*wd-dx, point2->corner[1]*ht-dy, point2->ctrl2[0]*wd-dx, point2->ctrl2[1]*ht-dy, point2->border[0]*MIN(wd,ht), point2->hardness, point2->density};
      float pc[7] = {point2->corner[0]*wd-dx, point2->corner[1]*ht-dy, point2->ctrl1[0]*wd-dx, point2->ctrl1[1]*ht-dy, point2->border[1]*MIN(wd,ht), point2->hardness, point2->density};
      float pd[7] = {point3->corner[0]*wd-dx, point3->corner[1]*ht-dy, point3->ctrl2[0]*wd-dx, point3->ctrl2[1]*ht-dy, point3->border[0]*MIN(wd,ht), point3->hardness, point3->density};
      memcpy(p1, pa, 7*sizeof(float));
      memcpy(p2, pb, 7*sizeof(float));
      memcpy(p3, pc, 7*sizeof(float));
      memcpy(p4, pd, 7*sizeof(float));
    }

    // 1st. special case: render abrupt transitions between different opacity and/or hardness values 
    if ((fabs(p1[5] - p2[5]) > 0.05f || fabs(p1[6] - p2[6]) > 0.05f) || (start_stamp && n == 2*nb-1))
    {
      if(n == 0)
      {
        start_stamp = 1;    // remember to deal with the first node as a final step
      }
      else
      {
        if (border)
        {
          float bmin[2] = { (*border)[posb-2], (*border)[posb-1] };
          float cmax[2] = { (*points)[pos-2], (*points)[pos-1] };
          _brush_points_stamp(cmax, bmin, *points, &pos, *border, &posb, TRUE);

          if (!_brush_buffer_grow(points, &pos, &points_max)) return 0;
          if (!_brush_buffer_grow(border, &posb, &border_max)) return 0;
        }

        if (payload)
        {
          for (int k = posp/2; k < pos/2; k++)
          {
            (*payload)[posp] = p1[5];
            (*payload)[posp+1] = p1[6];
            posp += 2;

            if (!_brush_buffer_grow(payload, &posp, &payload_max)) return 0;
          }
        }
      }
    }

    // 2nd. special case: render transition point between different brush sizes
    if (fabs(p1[4] - p2[4]) > 0.0001f && n > 0)
    {
      if (border)
      {
        float bmin[2] = { (*border)[posb-2], (*border)[posb-1] };
        float cmax[2] = { (*points)[pos-2], (*points)[pos-1] };
        float bmax[2] = { 2*cmax[0] - bmin[0], 2*cmax[1] - bmin[1] };
        _brush_points_recurs_border_gaps(cmax, bmin, NULL, bmax, *points, &pos, *border, &posb, TRUE);

        if (!_brush_buffer_grow(points, &pos, &points_max)) return 0;
        if (!_brush_buffer_grow(border, &posb, &border_max)) return 0;
      }

      if (payload)
      {
        for (int k = posp/2; k < pos/2; k++)
        {
          (*payload)[posp] = p1[5];
          (*payload)[posp+1] = p1[6];
          posp += 2;

          if (!_brush_buffer_grow(payload, &posp, &payload_max)) return 0;
        }
      }
    }

    // 3rd. special case: render endpoints
    if (k == k1)
    {
      if (border)
      {
        float bmin[2] = { (*border)[posb-2], (*border)[posb-1] };
        float cmax[2] = { (*points)[pos-2], (*points)[pos-1] };
        float bmax[2] = { 2*cmax[0] - bmin[0], 2*cmax[1] - bmin[1] };
        _brush_points_recurs_border_gaps(cmax, bmin, NULL, bmax, *points, &pos, *border, &posb, TRUE);

        if (!_brush_buffer_grow(points, &pos, &points_max)) return 0;
        if (!_brush_buffer_grow(border, &posb, &border_max)) return 0;
      }

      if (payload)
      {
        for (int k = posp/2; k < pos/2; k++)
        {
          (*payload)[posp] = p1[5];
          (*payload)[posp+1] = p1[6];
          posp += 2;

          if (!_brush_buffer_grow(payload, &posp, &payload_max)) return 0;
        }
      }

      cw *= -1;
      continue;
    }

    //and we determine all points by recursion (to be sure the distance between 2 points is <=1)
    float rc[2],rb[2],rp[2];
    float bmin[2] = {-99999,-99999};
    float bmax[2] = {-99999,-99999};
    float cmin[2] = {-99999,-99999};
    float cmax[2] = {-99999,-99999};

    if (border && payload) _brush_points_recurs(p1,p2,0.0,1.0,cmin,cmax,bmin,bmax,rc,rb,rp,*points,*border,*payload,&pos,&posb,&posp,TRUE,TRUE);
    else if (border) _brush_points_recurs(p1,p2,0.0,1.0,cmin,cmax,bmin,bmax,rc,rb,rp,*points,*border,NULL,&pos,&posb,&posp,TRUE,FALSE);
    else if (payload) _brush_points_recurs(p1,p2,0.0,1.0,cmin,cmax,bmin,bmax,rc,rb,rp,*points,NULL,*payload,&pos,&posb,&posp,FALSE,TRUE);
    else _brush_points_recurs(p1,p2,0.0,1.0,cmin,cmax,bmin,bmax,rc,rb,rp,*points,NULL,NULL,&pos,&posb,&posp,FALSE,FALSE);

    if (!_brush_buffer_grow(points, &pos, &points_max)) return 0;

    (*points)[pos++] = rc[0];
    (*points)[pos++] = rc[1];

    if (payload)
    {
      (*payload)[posp++] = rp[0];
      (*payload)[posp++] = rp[1];
      if (!_brush_buffer_grow(payload, &posp, &payload_max)) return 0;
    }

    if (border)
    {
      if (rb[0] == -9999999.0f)
      {
        if ((*border)[posb-2] == -9999999.0f)
        {
          (*border)[posb-2] = (*border)[posb-4];
          (*border)[posb-1] = (*border)[posb-3];
        }
        rb[0] = (*border)[posb-2];
        rb[1] = (*border)[posb-1];
      }
      (*border)[posb++] = rb[0];
      (*border)[posb++] = rb[1];

      if (!_brush_buffer_grow(border, &posb, &border_max)) return 0;
    }

    //we first want to be sure that there are no gaps in border
    if (border && nb>=3)
    {
      //we get the next point (start of the next segment)
      _brush_border_get_XY(p3[0],p3[1],p3[2],p3[3],p4[2],p4[3],p4[0],p4[1],0, p3[4],cmin,cmin+1,bmax,bmax+1);
      if (bmax[0] == -9999999.0f)
      {
        _brush_border_get_XY(p3[0],p3[1],p3[2],p3[3],p4[2],p4[3],p4[0],p4[1],0.0001, p3[4],cmin,cmin+1,bmax,bmax+1);
      }
      if (bmax[0]-rb[0] > 1 || bmax[0]-rb[0] < -1 || bmax[1]-rb[1] > 1 || bmax[1]-rb[1] < -1)
      {
        //float bmin2[2] = {(*border)[posb-22],(*border)[posb-21]};
        _brush_points_recurs_border_gaps(rc, rb, NULL, bmax, *points, &pos, *border, &posb, cw);
      }

      if (!_brush_buffer_grow(border, &posb, &border_max)) return 0;
    }

    if (payload)
    {
      for (int k = posp/2; k < pos/2; k++)
      {
        (*payload)[posp] = rp[0];
        (*payload)[posp+1] = rp[1];
        posp += 2;
      }
      if (!_brush_buffer_grow(payload, &posp, &payload_max)) return 0;
    }
  }
  *points_count = pos/2;
  if (border) *border_count = posb/2;
  if (payload) *payload_count = posp/2;

  //printf("points %d, border %d, playload %d\n", *points_count, border ? *border_count : -1, payload ? *payload_count : -1);

  if (darktable.unmuted & DT_DEBUG_PERF) dt_print(DT_DEBUG_MASKS, "[masks %s] brush_points point recurs %0.04f sec\n", form->name, dt_get_wtime()-start2);
  start2 = dt_get_wtime();

  //and we transform them with all distorted modules
  if (dt_dev_distort_transform_plus(dev,pipe,0,prio_max,*points,*points_count))
  {
    if (!border || dt_dev_distort_transform_plus(dev,pipe,0,prio_max,*border,*border_count))
    {
      if (darktable.unmuted & DT_DEBUG_PERF) dt_print(DT_DEBUG_MASKS, "[masks %s] brush_points transform took %0.04f sec\n", form->name, dt_get_wtime()-start2);
      start2 = dt_get_wtime();
      return 1;
    }
  }

  //if we failed, then free all and return
  free(*points);
  *points = NULL;
  *points_count = 0;

  if (border)
  {
    free(*border);
    *border = NULL;
    *border_count = 0;
  }

  if (payload)
  {
    free(*payload);
    *payload = NULL;
    *payload_count = 0;
  }
  return 0;
}

/** get the distance between point (x,y) and the brush */
static void dt_brush_get_distance(float x, int y, float as, dt_masks_form_gui_t *gui, int index, int corner_count, int *inside, int *inside_border, int *near, int *inside_source)
{
  if (!gui) return;
  //we first check if it's inside borders
  float as2 = as*as;

  int nb = 0;
  int last = -9999;
  int last2 = -9999;
  int lastw = -9999;
  int xx,yy;

  *inside = 0;
  *inside_border = 0;
  *inside_source = 0;
  *near = -1;

  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *) g_list_nth_data(gui->points,index);
  if (!gpt) return;

  //we first check if we are close to the source form
  if (gpt->source_count > corner_count*6 + 4)
  {
    for (int i=corner_count*6; i<gpt->source_count; i++)
    {
      float dd = (gpt->source[i*2]-x)*(gpt->source[i*2]-x)+(gpt->source[i*2+1]-y)*(gpt->source[i*2+1]-y);

      if(dd < as2)
      {
        *inside_source = 1;
        *inside = 1;
        *inside_border = 0;
        *near = -1;
        return;
      }
    }
  }

  //and we check if we are inside border
  if (gpt->border_count > corner_count*3)
  {
    for (int i=corner_count*3; i<gpt->border_count; i++)
    {
      xx = (int) gpt->border[i*2];
      yy = (int) gpt->border[i*2+1];
      if (xx == -999999)
      {
        if (yy==-999999) break;
        i = yy-1;
        continue;
      }
      //we check if we are at a point were the brush change of direction
      if (last2>0 && lastw>0 && lastw == last && yy != last)
      {
        if ((lastw-yy)*(lastw-last2)>0) nb++;
      }
      if (yy != last && (yy==y || (yy<last && y<last && y>yy) || (yy>last && last>0 && y>last && y<yy)))
      {
        if (xx > x)
        {
          nb++;
          lastw = yy;
        }
      }
      if (yy!=lastw) lastw = -999;
      if (yy!=last) last2 = last;
      last = yy;
    }
    xx = (int) gpt->border[corner_count*6];
    yy = (int) gpt->border[corner_count*6+1];
    if (xx == -999999)
    {
      xx = (int) gpt->border[(yy-1)*2];
      yy = (int) gpt->border[(yy-1)*2+1];
    }
    if ((yy-last>1 || yy-last<-1) && ((yy<last && y<last && y>yy) || (yy>last && last>0 && y>last && y<yy)) && xx>x) nb++;

    if (nb & 1)
    {
      *inside_border = 1;
      *inside = 0;
    }
  }

  //and we check if we are close to form
  int seg = 1;
  for (int i=corner_count*3; i<gpt->points_count; i++)
  {
    if (gpt->points[i*2+1] == gpt->points[seg*6+3] && gpt->points[i*2] == gpt->points[seg*6+2])
    {
      seg=(seg+1)%corner_count;
    }

    float dd = (gpt->points[i*2]-x)*(gpt->points[i*2]-x)+(gpt->points[i*2+1]-y)*(gpt->points[i*2+1]-y);

    if (dd < as2)
    {
      if (seg == 0) *near = corner_count-1;
      else *near = seg-1;

      *inside = 1;
      *inside_border = 0;
      return;
    }
  }
}

static int dt_brush_get_points_border(dt_develop_t *dev, dt_masks_form_t *form, float **points, int *points_count, float **border, int *border_count, int source)
{
  return _brush_get_points_border(dev,form,999999,dev->preview_pipe,points,points_count,border,border_count,NULL,NULL,source);
}

/** find relative position within a brush segment that is closest to the point given by coordinates x and y; 
    we only need to find the minimum with a resolution of 1%, so we just do an exhaustive search without any frills */
static float _brush_get_position_in_segment(float x, float y, dt_masks_form_t *form, int segment)
{
  int nb = g_list_length(form->points);
  int pos0 = segment;
  int pos1 = segment+1;
  int pos2 = segment+2;
  int pos3 = segment+3;

  dt_masks_point_brush_t *point0 = (dt_masks_point_brush_t *)g_list_nth_data(form->points, pos0);
  dt_masks_point_brush_t *point1 = pos1 < nb ? (dt_masks_point_brush_t *)g_list_nth_data(form->points, pos1) : point0;
  dt_masks_point_brush_t *point2 = pos2 < nb ? (dt_masks_point_brush_t *)g_list_nth_data(form->points, pos2) : point1;
  dt_masks_point_brush_t *point3 = pos3 < nb ? (dt_masks_point_brush_t *)g_list_nth_data(form->points, pos3) : point2;

  float tmin = 0;
  float dmin = FLT_MAX;

  for(float t = 0.0f; t <= 1.0f; t += 0.01f)
  {
    float sx, sy;
    _brush_get_XY(point0->corner[0], point0->corner[1], point1->corner[0], point1->corner[1],
                  point2->corner[0], point2->corner[1], point3->corner[0], point3->corner[1],
                  t, &sx, &sy);

    float d = (x - sx)*(x - sx)+(y - sy)*(y - sy);
    if(d < dmin)
    {
      dmin = d;
      tmin = t;
    }
  }

  return tmin;
}


static int dt_brush_events_mouse_scrolled(struct dt_iop_module_t *module, float pzx, float pzy, int up, uint32_t state,
    dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, int index)
{
  if (gui->creation)
  {
    if((state&GDK_SHIFT_MASK) == GDK_SHIFT_MASK)
    {
      float masks_hardness;
      float amount = 1.25f;
      if (up) amount = 0.8f;

      if (form->type & DT_MASKS_CLONE)
      {
        masks_hardness = dt_conf_get_float("plugins/darkroom/spots/brush_hardness");
        masks_hardness = MAX(0.05f, MIN(masks_hardness*amount, 1.0f));
        dt_conf_set_float("plugins/darkroom/spots/brush_hardness", masks_hardness);
      }
      else
      {
        masks_hardness = dt_conf_get_float("plugins/darkroom/masks/brush/hardness");
        masks_hardness = MAX(0.05f, MIN(masks_hardness*amount, 1.0f));
        dt_conf_set_float("plugins/darkroom/masks/brush/hardness", masks_hardness);
      }

      if (gui->guipoints_count > 0)
      {
        gui->guipoints_payload[4*(gui->guipoints_count-1)+1] = masks_hardness;
      }
    }
    else if((state&GDK_CONTROL_MASK) == GDK_CONTROL_MASK)
    {
      float masks_density;
      float amount = 1.25f;
      if (up) amount = 0.8f;

      if (form->type & DT_MASKS_CLONE)
      {
        masks_density = dt_conf_get_float("plugins/darkroom/spots/brush_density");
        masks_density = MAX(0.05f, MIN(masks_density*amount, 1.0f));
        dt_conf_set_float("plugins/darkroom/spots/brush_density", masks_density);
      }
      else
      {
        masks_density = dt_conf_get_float("plugins/darkroom/masks/brush/density");
        masks_density = MAX(0.05f, MIN(masks_density*amount, 1.0f));
        dt_conf_set_float("plugins/darkroom/masks/brush/density", masks_density);
      }

      if (gui->guipoints_count > 0)
      {
        gui->guipoints_payload[4*(gui->guipoints_count-1)+2] = masks_density;
      }
    }

    else
    {
      float masks_border;
      float amount = 1.03f;
      if (up) amount = 0.97f;

      if (form->type & DT_MASKS_CLONE)
      {
        masks_border = dt_conf_get_float("plugins/darkroom/spots/brush_border");
        masks_border = MAX(0.005f, MIN(masks_border*amount, 0.5f));
        dt_conf_set_float("plugins/darkroom/spots/brush_border", masks_border);
      }
      else
      {
        masks_border = dt_conf_get_float("plugins/darkroom/masks/brush/border");
        masks_border = MAX(0.005f, MIN(masks_border*amount, 0.5f));
        dt_conf_set_float("plugins/darkroom/masks/brush/border", masks_border);
      }

      if (gui->guipoints_count > 0)
      {
        gui->guipoints_payload[4*(gui->guipoints_count-1)] = masks_border;
      }
    }
    dt_control_queue_redraw_center();
    return 1;
  }
  else if (gui->form_selected || gui->point_selected >= 0 || gui->feather_selected >= 0 || gui->seg_selected >= 0)
  {
    //we register the current position
    if (gui->scrollx == 0.0f && gui->scrolly == 0.0f)
    {
      gui->scrollx = pzx;
      gui->scrolly = pzy;
    }
    if ((state&GDK_CONTROL_MASK) == GDK_CONTROL_MASK)
    {
      //we try to change the opacity
      dt_masks_form_change_opacity(form,parentid,up);
    }
    else
    {
      int nb = g_list_length(form->points);
      if (gui->border_selected)
      {
        float amount = 1.03f;
        if (up) amount = 0.97f;
        for(int k = 0; k < nb; k++)
        {
          dt_masks_point_brush_t *point = (dt_masks_point_brush_t *)g_list_nth_data(form->points,k);
          point->border[0] *= amount;
          point->border[1] *= amount;
        }
        if (form->type & DT_MASKS_CLONE)
        {
          float masks_border = dt_conf_get_float("plugins/darkroom/spots/brush_border");
          masks_border = MAX(0.005f, MIN(masks_border*amount, 0.5f));
          dt_conf_set_float("plugins/darkroom/spots/brush_border", masks_border);
        }
        else
        {
          float masks_border = dt_conf_get_float("plugins/darkroom/masks/brush/border");
          masks_border = MAX(0.005f, MIN(masks_border*amount, 0.5f));
          dt_conf_set_float("plugins/darkroom/masks/brush/border", masks_border*amount);
        }
      }
      else
      {
        float amount = 1.25f;
        if (up) amount = 0.8f;
        for(int k = 0; k < nb; k++)
        {
          dt_masks_point_brush_t *point = (dt_masks_point_brush_t *)g_list_nth_data(form->points,k);
          float masks_hardness = point->hardness;
          point->hardness = MAX(0.05f, MIN(masks_hardness*amount, 1.0f));
        }
        if (form->type & DT_MASKS_CLONE)
        {
          float masks_hardness = dt_conf_get_float("plugins/darkroom/spots/brush_hardness");
          masks_hardness = MAX(0.05f, MIN(masks_hardness*amount, 1.0f));
          dt_conf_set_float("plugins/darkroom/spots/brush_hardness", masks_hardness);
        }
        else
        {
          float masks_hardness = dt_conf_get_float("plugins/darkroom/masks/brush/hardness");
          masks_hardness = MAX(0.05f, MIN(masks_hardness*amount, 1.0f));
          dt_conf_set_float("plugins/darkroom/masks/brush/hardness", masks_hardness);
        }
      }

      dt_masks_write_form(form,darktable.develop);

      //we recreate the form points
      dt_masks_gui_form_remove(form,gui,index);
      dt_masks_gui_form_create(form,gui,index);

      //we save the move
      dt_masks_update_image(darktable.develop);
    }
    return 1;
  }
  return 0;
}

static int dt_brush_events_button_pressed(struct dt_iop_module_t *module, float pzx, float pzy, double pressure, int which, int type, uint32_t state,
    dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, int index)
{
  if (type==GDK_2BUTTON_PRESS || type==GDK_3BUTTON_PRESS) return 1;
  if (!gui) return 0;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *) g_list_nth_data(gui->points,index);
  if (!gpt) return 0;

  float masks_border;
  if (form->type & DT_MASKS_CLONE) masks_border = MIN(dt_conf_get_float("plugins/darkroom/spots/brush_border"),0.5f);
  else masks_border = MIN(dt_conf_get_float("plugins/darkroom/masks/brush/border"),0.5f);

  float masks_hardness;
  if (form->type & DT_MASKS_CLONE) masks_hardness = MIN(dt_conf_get_float("plugins/darkroom/spots/brush_hardness"),1.0f);
  else masks_hardness = MIN(dt_conf_get_float("plugins/darkroom/masks/brush/hardness"),1.0f);

  float masks_density;
  if (form->type & DT_MASKS_CLONE) masks_density = MIN(dt_conf_get_float("plugins/darkroom/spots/brush_density"),1.0f);
  else masks_density = MIN(dt_conf_get_float("plugins/darkroom/masks/brush/density"),1.0f);

  if (which == 1)
  {
    if (gui->creation)
    {
      float wd = darktable.develop->preview_pipe->backbuf_width;
      float ht = darktable.develop->preview_pipe->backbuf_height;

      if(!gui->guipoints) gui->guipoints = malloc(200000*sizeof(float));
      if(!gui->guipoints) return 1;
      if(!gui->guipoints_payload) gui->guipoints_payload = malloc(400000*sizeof(float));
      if(!gui->guipoints_payload) return 1;
      gui->guipoints[0] = pzx*wd;
      gui->guipoints[1] = pzy*ht;
      gui->guipoints_payload[0] = masks_border;
      gui->guipoints_payload[1] = masks_hardness;
      gui->guipoints_payload[2] = masks_density;
      gui->guipoints_payload[3] = pressure;

      gui->guipoints_count = 1;

      gui->pressure_sensitivity = DT_MASKS_PRESSURE_OFF;
      char *psens = dt_conf_get_string("pressure_sensitivity");
      if(psens)
      {
        if(!strcmp(psens, "hardness (absolute)"))
          gui->pressure_sensitivity = DT_MASKS_PRESSURE_HARDNESS_ABS;
        else if(!strcmp(psens, "hardness (relative)"))
          gui->pressure_sensitivity = DT_MASKS_PRESSURE_HARDNESS_REL;
        else if(!strcmp(psens, "opacity (absolute)"))
          gui->pressure_sensitivity = DT_MASKS_PRESSURE_OPACITY_ABS;
        else if(!strcmp(psens, "opacity (relative)"))
          gui->pressure_sensitivity = DT_MASKS_PRESSURE_OPACITY_REL;
        else if(!strcmp(psens, "brush size (relative)"))
          gui->pressure_sensitivity = DT_MASKS_PRESSURE_BRUSHSIZE_REL;
      }

      dt_control_queue_redraw_center();
      return 1;
    }
    else if (gui->source_selected && gui->edit_mode == DT_MASKS_EDIT_FULL)
    {
      dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *) g_list_nth_data(gui->points,index);
      if (!gpt) return 0;
      //we start the form dragging
      gui->source_dragging = TRUE;
      gui->posx = pzx*darktable.develop->preview_pipe->backbuf_width;
      gui->posy = pzy*darktable.develop->preview_pipe->backbuf_height;
      gui->dx = gpt->source[2] - gui->posx;
      gui->dy = gpt->source[3] - gui->posy;
      return 1;
    }
    else if (gui->form_selected && gui->edit_mode == DT_MASKS_EDIT_FULL)
    {
      gui->form_dragging = TRUE;
      gui->point_edited = -1;
      gui->posx = pzx*darktable.develop->preview_pipe->backbuf_width;
      gui->posy = pzy*darktable.develop->preview_pipe->backbuf_height;
      gui->dx = gpt->points[2] - gui->posx;
      gui->dy = gpt->points[3] - gui->posy;
      return 1;
    }
    else if (gui->point_selected >= 0)
    {
      //if ctrl is pressed, we change the type of point
      if (gui->point_edited==gui->point_selected && ((state&GDK_CONTROL_MASK) == GDK_CONTROL_MASK))
      {
        dt_masks_point_brush_t *point = (dt_masks_point_brush_t *)g_list_nth_data(form->points,gui->point_edited);
        if (point->state != DT_MASKS_POINT_STATE_NORMAL)
        {
          point->state = DT_MASKS_POINT_STATE_NORMAL;
          _brush_init_ctrl_points(form);
        }
        else
        {
          point->ctrl1[0] = point->ctrl2[0] = point->corner[0];
          point->ctrl1[1] = point->ctrl2[1] = point->corner[1];
          point->state = DT_MASKS_POINT_STATE_USER;
        }
        dt_masks_write_form(form,darktable.develop);

        //we recreate the form points
        dt_masks_gui_form_remove(form,gui,index);
        dt_masks_gui_form_create(form,gui,index);
        //we save the move
        dt_masks_update_image(darktable.develop);
        return 1;
      }
      //we register the current position to avoid accidental move
      if (gui->point_edited<0 && gui->scrollx == 0.0f && gui->scrolly == 0.0f)
      {
        gui->scrollx = pzx;
        gui->scrolly = pzy;
      }
      gui->point_edited = gui->point_dragging  = gui->point_selected;
      dt_control_queue_redraw_center();
      return 1;
    }
    else if (gui->feather_selected >= 0)
    {
      gui->feather_dragging = gui->feather_selected;
      dt_control_queue_redraw_center();
      return 1;
    }
    else if (gui->point_border_selected >= 0)
    {
      gui->point_edited = -1;
      gui->point_border_dragging = gui->point_border_selected;
      dt_control_queue_redraw_center();
      return 1;
    }
    else if (gui->seg_selected >= 0)
    {
      int nb = g_list_length(form->points);
      gui->point_edited = -1;
      if ((state&GDK_CONTROL_MASK) == GDK_CONTROL_MASK)
      {
        //we add a new point to the brush
        dt_masks_point_brush_t *bzpt = (dt_masks_point_brush_t *) (malloc(sizeof(dt_masks_point_brush_t)));

        float wd = darktable.develop->preview_pipe->backbuf_width;
        float ht = darktable.develop->preview_pipe->backbuf_height;
        float pts[2] = {pzx*wd,pzy*ht};
        dt_dev_distort_backtransform(darktable.develop,pts,1);

        //set coordinates
        bzpt->corner[0] = pts[0]/darktable.develop->preview_pipe->iwidth;
        bzpt->corner[1] = pts[1]/darktable.develop->preview_pipe->iheight;
        bzpt->ctrl1[0] = bzpt->ctrl1[1] = bzpt->ctrl2[0] = bzpt->ctrl2[1] = -1.0;
        bzpt->state = DT_MASKS_POINT_STATE_NORMAL;

        //set other attributes of the new point. we interpolate the starting and the end point of that segment
        float t = _brush_get_position_in_segment(bzpt->corner[0], bzpt->corner[1], form, gui->seg_selected);
        //start and end point of the segment
        dt_masks_point_brush_t *point0 = (dt_masks_point_brush_t *)g_list_nth_data(form->points,gui->seg_selected);
        dt_masks_point_brush_t *point1 = (dt_masks_point_brush_t *)g_list_nth_data(form->points,gui->seg_selected+1);
        bzpt->border[0] = point0->border[0]*(1.0f - t) + point1->border[0]*t;
        bzpt->border[1] = point0->border[1]*(1.0f - t) + point1->border[1]*t;
        bzpt->hardness = point0->hardness*(1.0f - t) + point1->hardness*t;
        bzpt->density = point0->density*(1.0f - t) + point1->density*t;

        form->points = g_list_insert(form->points,bzpt,gui->seg_selected+1);
        _brush_init_ctrl_points(form);
        dt_masks_gui_form_remove(form,gui,index);
        dt_masks_gui_form_create(form,gui,index);
        gui->point_edited = gui->point_dragging  = gui->point_selected = gui->seg_selected+1;
        gui->seg_selected = -1;
        dt_control_queue_redraw_center();
      }
      else if (gui->seg_selected > 0 && gui->seg_selected < nb - 1)
      {
        //we move the entire segment but only if it's not the first or the last one
        gui->seg_dragging = gui->seg_selected;
        gui->posx = pzx*darktable.develop->preview_pipe->backbuf_width;
        gui->posy = pzy*darktable.develop->preview_pipe->backbuf_height;
        gui->dx = gpt->points[gui->seg_selected*6+2] - gui->posx;
        gui->dy = gpt->points[gui->seg_selected*6+3] - gui->posy;
      }
      return 1;
    }
    gui->point_edited = -1;
  }
  else if (which==3 && parentid>0 && gui->edit_mode == DT_MASKS_EDIT_FULL)
  {
    dt_masks_clear_form_gui(darktable.develop);
    //we hide the form
    if (!(darktable.develop->form_visible->type & DT_MASKS_GROUP)) darktable.develop->form_visible = NULL;
    else if (g_list_length(darktable.develop->form_visible->points) < 2) darktable.develop->form_visible = NULL;
    else
    {
      GList *forms = g_list_first(darktable.develop->form_visible->points);
      while (forms)
      {
        dt_masks_point_group_t *gpt = (dt_masks_point_group_t *)forms->data;
        if (gpt->formid == form->formid)
        {
          darktable.develop->form_visible->points = g_list_remove(darktable.develop->form_visible->points,gpt);
          break;
        }
        forms = g_list_next(forms);
      }
    }

    //we remove the shape
    dt_masks_form_remove(module,dt_masks_get_from_id(darktable.develop,parentid),form);
    dt_dev_masks_list_remove(darktable.develop,form->formid,parentid);
    return 1;
  }

  return 0;
}

static int dt_brush_events_button_released(struct dt_iop_module_t *module,float pzx, float pzy, int which, uint32_t state,
    dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui, int index)
{
  if (!gui) return 0;

  // disable pressure readings
  dt_gui_disable_extended_input_devices();

  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *) g_list_nth_data(gui->points,index);
  if (!gpt) return 0;

  float masks_border;
  if (form->type & DT_MASKS_CLONE) masks_border = MIN(dt_conf_get_float("plugins/darkroom/spots/brush_border"),0.5f);
  else masks_border = MIN(dt_conf_get_float("plugins/darkroom/masks/brush/border"),0.5f);

  float masks_hardness;
  if (form->type & DT_MASKS_CLONE) masks_hardness = MIN(dt_conf_get_float("plugins/darkroom/spots/brush_hardness"),1.0f);
  else masks_hardness = MIN(dt_conf_get_float("plugins/darkroom/masks/brush/hardness"),1.0f);

  if (gui->creation)
  {
    dt_iop_module_t *crea_module = gui->creation_module;

    if(gui->guipoints && gui->guipoints_count > 0)
    {

      //if the path consists only of one x/y pair we add a second one close so we don't need to deal with this special case later
      if(gui->guipoints_count == 1)
      {
        // add a helper node very close to the single spot
        gui->guipoints[2] = gui->guipoints[0]+0.01f;
        gui->guipoints[3] = gui->guipoints[1]-0.01f;
        gui->guipoints_payload[4] = gui->guipoints_payload[0];
        gui->guipoints_payload[5] = gui->guipoints_payload[1];
        gui->guipoints_payload[6] = gui->guipoints_payload[2];
        gui->guipoints_payload[7] = gui->guipoints_payload[3];
        gui->guipoints_count++;
      }

      //we transform the points
      dt_dev_distort_backtransform(darktable.develop, gui->guipoints, gui->guipoints_count);

      for(int i=0; i < gui->guipoints_count; i++)
      {
        gui->guipoints[i*2] /= darktable.develop->preview_pipe->iwidth;
        gui->guipoints[i*2+1] /= darktable.develop->preview_pipe->iheight;
      }

      const float epsilon2 = MAX(0.005f, masks_border)*MAX(0.005f, masks_border)*masks_hardness*masks_hardness*0.05f;

      //we simplify the path and generate the nodes
      form->points = _brush_ramer_douglas_peucker(gui->guipoints, gui->guipoints_count, gui->guipoints_payload, epsilon2, gui->pressure_sensitivity);

      //printf("guipoints_count %d, points %d\n", gui->guipoints_count, g_list_length(form->points));

      _brush_init_ctrl_points(form);

      free(gui->guipoints);
      free(gui->guipoints_payload);
      gui->guipoints = NULL;
      gui->guipoints_payload = NULL;
      gui->guipoints_count = 0;

      //we save the form and quit creation mode
      dt_masks_gui_form_save_creation(crea_module,form,gui);
      if (crea_module)
      {
        dt_dev_add_history_item(darktable.develop, crea_module, TRUE);
        //and we switch in edit mode to show all the forms
        dt_masks_set_edit_mode(crea_module, TRUE);
        dt_masks_iop_update(crea_module);
        gui->creation_module = NULL;
      }
      else
      {
        dt_dev_masks_selection_change(darktable.develop,form->formid, TRUE);
      }
    }
    else
    {
      free(gui->guipoints);
      free(gui->guipoints_payload);
      gui->guipoints = NULL;
      gui->guipoints_payload = NULL;
      gui->guipoints_count = 0;

      //we remove the form
      dt_masks_free_form(form);
      darktable.develop->form_visible = NULL;
      dt_masks_clear_form_gui(darktable.develop);
    }

    dt_control_queue_redraw_center();
    return 1;
  }
  else if (gui->form_dragging)
  {
    //we end the form dragging
    gui->form_dragging = FALSE;

    //we get point0 new values
    dt_masks_point_brush_t *point = (dt_masks_point_brush_t *)g_list_first(form->points)->data;
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = {pzx*wd+gui->dx,pzy*ht+gui->dy};
    dt_dev_distort_backtransform(darktable.develop,pts,1);
    float dx = pts[0]/darktable.develop->preview_pipe->iwidth - point->corner[0];
    float dy = pts[1]/darktable.develop->preview_pipe->iheight - point->corner[1];

    //we move all points
    GList *points = g_list_first(form->points);
    while (points)
    {
      point = (dt_masks_point_brush_t *)points->data;
      point->corner[0] += dx;
      point->corner[1] += dy;
      point->ctrl1[0] += dx;
      point->ctrl1[1] += dy;
      point->ctrl2[0] += dx;
      point->ctrl2[1] += dy;
      points = g_list_next(points);
    }

    dt_masks_write_form(form,darktable.develop);

    //we recreate the form points
    dt_masks_gui_form_remove(form,gui,index);
    dt_masks_gui_form_create(form,gui,index);

    //we save the move
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if (gui->source_dragging)
  {
    //we end the form dragging
    gui->source_dragging = FALSE;

    //we change the source value
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = {pzx*wd+gui->dx,pzy*ht+gui->dy};
    dt_dev_distort_backtransform(darktable.develop,pts,1);
    form->source[0] = pts[0]/darktable.develop->preview_pipe->iwidth;
    form->source[1] = pts[1]/darktable.develop->preview_pipe->iheight;
    dt_masks_write_form(form,darktable.develop);

    //we recreate the form points
    dt_masks_gui_form_remove(form,gui,index);
    dt_masks_gui_form_create(form,gui,index);

    //we save the move
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if (gui->seg_dragging>=0)
  {
    gui->seg_dragging = -1;
    dt_masks_write_form(form,darktable.develop);
    dt_masks_update_image(darktable.develop);
    return 1;
  }
  else if (gui->point_dragging >= 0)
  {
    dt_masks_point_brush_t *point = (dt_masks_point_brush_t *)g_list_nth_data(form->points,gui->point_dragging);
    gui->point_dragging = -1;
    if (gui->scrollx != 0.0f || gui->scrolly != 0.0f)
    {
      gui->scrollx = gui->scrolly = 0;
      return 1;
    }
    gui->scrollx = gui->scrolly = 0;
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = {pzx*wd,pzy*ht};
    dt_dev_distort_backtransform(darktable.develop,pts,1);
    float dx = pts[0]/darktable.develop->preview_pipe->iwidth - point->corner[0];
    float dy = pts[1]/darktable.develop->preview_pipe->iheight - point->corner[1];

    point->corner[0] += dx;
    point->corner[1] += dy;
    point->ctrl1[0] += dx;
    point->ctrl1[1] += dy;
    point->ctrl2[0] += dx;
    point->ctrl2[1] += dy;

    _brush_init_ctrl_points(form);

    dt_masks_write_form(form,darktable.develop);

    //we recreate the form points
    dt_masks_gui_form_remove(form,gui,index);
    dt_masks_gui_form_create(form,gui,index);
    //we save the move
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if (gui->feather_dragging >= 0)
  {
    dt_masks_point_brush_t *point = (dt_masks_point_brush_t *)g_list_nth_data(form->points,gui->feather_dragging);
    gui->feather_dragging = -1;
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = {pzx*wd,pzy*ht};
    dt_dev_distort_backtransform(darktable.develop,pts,1);

    int p1x,p1y,p2x,p2y;
    _brush_feather_to_ctrl(point->corner[0]*darktable.develop->preview_pipe->iwidth,point->corner[1]*darktable.develop->preview_pipe->iheight,pts[0],pts[1],
                           &p1x,&p1y,&p2x,&p2y,TRUE);
    point->ctrl1[0] = (float)p1x/darktable.develop->preview_pipe->iwidth;
    point->ctrl1[1] = (float)p1y/darktable.develop->preview_pipe->iheight;
    point->ctrl2[0] = (float)p2x/darktable.develop->preview_pipe->iwidth;
    point->ctrl2[1] = (float)p2y/darktable.develop->preview_pipe->iheight;

    point->state = DT_MASKS_POINT_STATE_USER;

    _brush_init_ctrl_points(form);

    dt_masks_write_form(form,darktable.develop);

    //we recreate the form points
    dt_masks_gui_form_remove(form,gui,index);
    dt_masks_gui_form_create(form,gui,index);
    //we save the move
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if (gui->point_border_dragging >= 0)
  {
    gui->point_border_dragging = -1;

    //we save the move
    dt_masks_write_form(form,darktable.develop);
    dt_masks_update_image(darktable.develop);
    dt_control_queue_redraw_center();
    return 1;
  }
  else if (gui->point_selected>=0 && which == 3)
  {
    //we remove the point (and the entire form if there is too few points)
    if (g_list_length(form->points) < 2)
    {
      dt_masks_clear_form_gui(darktable.develop);
      //we hide the form
      if (!(darktable.develop->form_visible->type & DT_MASKS_GROUP)) darktable.develop->form_visible = NULL;
      else if (g_list_length(darktable.develop->form_visible->points) < 2) darktable.develop->form_visible = NULL;
      else
      {
        GList *forms = g_list_first(darktable.develop->form_visible->points);
        while (forms)
        {
          dt_masks_point_group_t *gpt = (dt_masks_point_group_t *)forms->data;
          if (gpt->formid == form->formid)
          {
            darktable.develop->form_visible->points = g_list_remove(darktable.develop->form_visible->points,gpt);
            break;
          }
          forms = g_list_next(forms);
        }
      }

      //we delete or remove the shape
      dt_masks_form_remove(module,NULL,form);
      dt_dev_masks_list_change(darktable.develop);
      dt_control_queue_redraw_center();
      return 1;
    }
    form->points = g_list_delete_link(form->points,g_list_nth(form->points,gui->point_selected));
    gui->point_selected = -1;
    _brush_init_ctrl_points(form);

    dt_masks_write_form(form,darktable.develop);

    //we recreate the form points
    dt_masks_gui_form_remove(form,gui,index);
    dt_masks_gui_form_create(form,gui,index);
    //we save the move
    dt_masks_update_image(darktable.develop);

    return 1;
  }
  else if (gui->feather_selected>=0 && which == 3)
  {
    dt_masks_point_brush_t *point = (dt_masks_point_brush_t *)g_list_nth_data(form->points,gui->feather_selected);
    if (point->state != DT_MASKS_POINT_STATE_NORMAL)
    {
      point->state = DT_MASKS_POINT_STATE_NORMAL;
      _brush_init_ctrl_points(form);

      dt_masks_write_form(form,darktable.develop);

      //we recreate the form points
      dt_masks_gui_form_remove(form,gui,index);
      dt_masks_gui_form_create(form,gui,index);
      //we save the move
      dt_masks_update_image(darktable.develop);
    }
    return 1;
  }

  return 0;
}

static int dt_brush_events_mouse_moved(struct dt_iop_module_t *module, float pzx, float pzy, double pressure, int which, dt_masks_form_t *form, int parentid, dt_masks_form_gui_t *gui,int index)
{
  int32_t zoom, closeup;
  DT_CTL_GET_GLOBAL(zoom, dev_zoom);
  DT_CTL_GET_GLOBAL(closeup, dev_closeup);
  float zoom_scale = dt_dev_get_zoom_scale(darktable.develop, zoom, closeup ? 2 : 1, 1);
  float as = 0.005f/zoom_scale*darktable.develop->preview_pipe->backbuf_width;
  if (!gui) return 0;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *) g_list_nth_data(gui->points,index);
  if (!gpt) return 0;

  if (gui->creation)
  {
    if(gui->guipoints && gui->guipoints_count < 100000)
    {
      gui->guipoints[2*gui->guipoints_count] = pzx*darktable.develop->preview_pipe->backbuf_width;
      gui->guipoints[2*gui->guipoints_count+1] = pzy*darktable.develop->preview_pipe->backbuf_height;
      gui->guipoints_payload[4*gui->guipoints_count] = gui->guipoints_payload[4*(gui->guipoints_count-1)];
      gui->guipoints_payload[4*gui->guipoints_count+1] = gui->guipoints_payload[4*(gui->guipoints_count-1)+1];
      gui->guipoints_payload[4*gui->guipoints_count+2] = gui->guipoints_payload[4*(gui->guipoints_count-1)+2];
      gui->guipoints_payload[4*gui->guipoints_count+3] = pressure;
      gui->guipoints_count++;
    }
    else
    {
      gui->posx = pzx*darktable.develop->preview_pipe->backbuf_width;
      gui->posy = pzy*darktable.develop->preview_pipe->backbuf_height;
    }
    dt_control_queue_redraw_center();
    return 1;
  }
  else if (gui->point_dragging >= 0)
  {
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = {pzx*wd,pzy*ht};
    dt_dev_distort_backtransform(darktable.develop,pts,1);
    dt_masks_point_brush_t *bzpt = (dt_masks_point_brush_t *)g_list_nth_data(form->points,gui->point_dragging);
    pzx = pts[0]/darktable.develop->preview_pipe->iwidth;
    pzy = pts[1]/darktable.develop->preview_pipe->iheight;
    bzpt->ctrl1[0] += pzx - bzpt->corner[0];
    bzpt->ctrl2[0] += pzx - bzpt->corner[0];
    bzpt->ctrl1[1] += pzy - bzpt->corner[1];
    bzpt->ctrl2[1] += pzy - bzpt->corner[1];
    bzpt->corner[0] = pzx;
    bzpt->corner[1] = pzy;
    _brush_init_ctrl_points(form);
    //we recreate the form points
    dt_masks_gui_form_remove(form,gui,index);
    dt_masks_gui_form_create(form,gui,index);
    dt_control_queue_redraw_center();
    return 1;
  }
  else if (gui->seg_dragging >= 0)
  {
    //we get point0 new values
    int pos2 = (gui->seg_dragging+1)%g_list_length(form->points);
    dt_masks_point_brush_t *point = (dt_masks_point_brush_t *)g_list_nth_data(form->points,gui->seg_dragging);
    dt_masks_point_brush_t *point2 = (dt_masks_point_brush_t *)g_list_nth_data(form->points,pos2);
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = {pzx*wd+gui->dx,pzy*ht+gui->dy};
    dt_dev_distort_backtransform(darktable.develop,pts,1);
    float dx = pts[0]/darktable.develop->preview_pipe->iwidth - point->corner[0];
    float dy = pts[1]/darktable.develop->preview_pipe->iheight - point->corner[1];

    //we move all points
    point->corner[0] += dx;
    point->corner[1] += dy;
    point->ctrl1[0] += dx;
    point->ctrl1[1] += dy;
    point->ctrl2[0] += dx;
    point->ctrl2[1] += dy;
    point2->corner[0] += dx;
    point2->corner[1] += dy;
    point2->ctrl1[0] += dx;
    point2->ctrl1[1] += dy;
    point2->ctrl2[0] += dx;
    point2->ctrl2[1] += dy;

    _brush_init_ctrl_points(form);

    dt_masks_write_form(form,darktable.develop);

    //we recreate the form points
    dt_masks_gui_form_remove(form,gui,index);
    dt_masks_gui_form_create(form,gui,index);

    dt_control_queue_redraw_center();
    return 1;
  }
  else if (gui->feather_dragging >= 0)
  {
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;
    float pts[2] = {pzx*wd,pzy*ht};
    dt_dev_distort_backtransform(darktable.develop,pts,1);
    dt_masks_point_brush_t *point = (dt_masks_point_brush_t *)g_list_nth_data(form->points,gui->feather_dragging);

    int p1x,p1y,p2x,p2y;
    _brush_feather_to_ctrl(point->corner[0]*darktable.develop->preview_pipe->iwidth,point->corner[1]*darktable.develop->preview_pipe->iheight,pts[0],pts[1],
                           &p1x,&p1y,&p2x,&p2y,TRUE);
    point->ctrl1[0] = (float)p1x/darktable.develop->preview_pipe->iwidth;
    point->ctrl1[1] = (float)p1y/darktable.develop->preview_pipe->iheight;
    point->ctrl2[0] = (float)p2x/darktable.develop->preview_pipe->iwidth;
    point->ctrl2[1] = (float)p2y/darktable.develop->preview_pipe->iheight;
    point->state = DT_MASKS_POINT_STATE_USER;

    _brush_init_ctrl_points(form);
    //we recreate the form points
    dt_masks_gui_form_remove(form,gui,index);
    dt_masks_gui_form_create(form,gui,index);
    dt_control_queue_redraw_center();
    return 1;
  }
  else if (gui->point_border_dragging >= 0)
  {
    float wd = darktable.develop->preview_pipe->backbuf_width;
    float ht = darktable.develop->preview_pipe->backbuf_height;

    int k = gui->point_border_dragging;

    //now we want to know the position reflected on actual corner/border segment
    float a = (gpt->border[k*6+1]-gpt->points[k*6+3])/(float)(gpt->border[k*6]-gpt->points[k*6+2]);
    float b = gpt->points[k*6+3]-a*gpt->points[k*6+2];

    float pts[2];
    pts[0] = (a*pzy*ht+pzx*wd-b*a)/(a*a+1.0);
    pts[1] = a*pts[0]+b;

    dt_dev_distort_backtransform(darktable.develop,pts,1);

    dt_masks_point_brush_t *point = (dt_masks_point_brush_t *)g_list_nth_data(form->points,k);
    float nx = point->corner[0]*darktable.develop->preview_pipe->iwidth;
    float ny = point->corner[1]*darktable.develop->preview_pipe->iheight;
    float nr = sqrtf((pts[0]-nx)*(pts[0]-nx) + (pts[1]-ny)*(pts[1]-ny));
    float bdr = nr/fminf(darktable.develop->preview_pipe->iwidth,darktable.develop->preview_pipe->iheight);

    point->border[0] = point->border[1] = bdr;

    //we recreate the form points
    dt_masks_gui_form_remove(form,gui,index);
    dt_masks_gui_form_create(form,gui,index);
    dt_control_queue_redraw_center();
    return 1;
  }
  else if (gui->form_dragging || gui->source_dragging)
  {
    gui->posx = pzx*darktable.develop->preview_pipe->backbuf_width;
    gui->posy = pzy*darktable.develop->preview_pipe->backbuf_height;
    dt_control_queue_redraw_center();
    return 1;
  }

  gui->form_selected = FALSE;
  gui->border_selected = FALSE;
  gui->source_selected = FALSE;
  gui->feather_selected  = -1;
  gui->point_selected = -1;
  gui->seg_selected = -1;
  gui->point_border_selected = -1;
  //are we near a point or feather ?
  int nb = g_list_length(form->points);

  pzx *= darktable.develop->preview_pipe->backbuf_width, pzy *= darktable.develop->preview_pipe->backbuf_height;

  if ((gui->group_selected == index) && gui->point_edited >= 0)
  {
    int k = gui->point_edited;
    //we only select feather if the point is not "sharp"
    if (gpt->points[k*6+2]!=gpt->points[k*6+4] && gpt->points[k*6+3]!=gpt->points[k*6+5])
    {
      int ffx,ffy;
      _brush_ctrl2_to_feather(gpt->points[k*6+2],gpt->points[k*6+3],gpt->points[k*6+4],gpt->points[k*6+5],&ffx,&ffy,TRUE);
      if (pzx-ffx>-as && pzx-ffx<as && pzy-ffy>-as && pzy-ffy<as)
      {
        gui->feather_selected = k;
        dt_control_queue_redraw_center();
        return 1;
      }
    }
    //corner ??
    if (pzx-gpt->points[k*6+2]>-as && pzx-gpt->points[k*6+2]<as && pzy-gpt->points[k*6+3]>-as && pzy-gpt->points[k*6+3]<as)
    {
      gui->point_selected = k;
      dt_control_queue_redraw_center();
      return 1;
    }
  }

  for (int k=0; k<nb; k++)
  {
    //corner ??
    if (pzx-gpt->points[k*6+2]>-as && pzx-gpt->points[k*6+2]<as && pzy-gpt->points[k*6+3]>-as && pzy-gpt->points[k*6+3]<as)
    {
      gui->point_selected = k;
      dt_control_queue_redraw_center();
      return 1;
    }

    //border corner ??
    if (pzx-gpt->border[k*6]>-as && pzx-gpt->border[k*6]<as && pzy-gpt->border[k*6+1]>-as && pzy-gpt->border[k*6+1]<as)
    {
      gui->point_border_selected = k;
      dt_control_queue_redraw_center();
      return 1;
    }
  }

  //are we inside the form or the borders or near a segment ???
  int in, inb, near, ins;
  dt_brush_get_distance(pzx,(int)pzy,as,gui,index,nb,&in,&inb,&near,&ins);
  gui->seg_selected = near;
  if (near<0)
  {
    if (ins)
    {
      gui->form_selected = TRUE;
      gui->source_selected = TRUE;
    }
    else if (in)
    {
      gui->form_selected = TRUE;
    }
    else if (inb)
    {
      gui->form_selected = TRUE;
      gui->border_selected = TRUE;
    }
  }
  dt_control_queue_redraw_center();
  if (!gui->form_selected && !gui->border_selected && gui->seg_selected<0) return 0;
  if (gui->edit_mode != DT_MASKS_EDIT_FULL) return 0;
  return 1;
}

static void dt_brush_events_post_expose(cairo_t *cr, float zoom_scale, dt_masks_form_gui_t *gui, int index, int nb)
{
  double dashed[] = {4.0, 4.0};
  dashed[0] /= zoom_scale;
  dashed[1] /= zoom_scale;
  int len  = sizeof(dashed) / sizeof(dashed[0]);

  if (!gui) return;
  dt_masks_form_gui_points_t *gpt = (dt_masks_form_gui_points_t *) g_list_nth_data(gui->points,index);
  if (!gpt) return;
  float dx=0, dy=0, dxs=0, dys=0;
  if ((gui->group_selected == index) && gui->form_dragging)
  {
    dx = gui->posx + gui->dx - gpt->points[2];
    dy = gui->posy + gui->dy - gpt->points[3];
  }
  if ((gui->group_selected == index) && gui->source_dragging)
  {
    dxs = gui->posx + gui->dx - gpt->source[2];
    dys = gui->posy + gui->dy - gpt->source[3];
  }

  //in creation mode
  if(gui->creation)
  {
    float wd = darktable.develop->preview_pipe->iwidth;
    float ht = darktable.develop->preview_pipe->iheight;

    if(gui->guipoints_count == 0)
    {
      dt_masks_form_t *form = darktable.develop->form_visible;
      if(!form) return;

      float masks_border;
      if (form->type & DT_MASKS_CLONE) masks_border = MIN(dt_conf_get_float("plugins/darkroom/spots/brush_border"),0.5f);
      else masks_border = MIN(dt_conf_get_float("plugins/darkroom/masks/brush/border"),0.5f);

      float masks_hardness;
      if (form->type & DT_MASKS_CLONE) masks_hardness = MIN(dt_conf_get_float("plugins/darkroom/spots/brush_hardness"),1.0f);
      else masks_hardness = MIN(dt_conf_get_float("plugins/darkroom/masks/brush/hardness"),1.0f);

      float masks_density;
      if (form->type & DT_MASKS_CLONE) masks_density = MIN(dt_conf_get_float("plugins/darkroom/spots/brush_density"),1.0f);
      else masks_density = MIN(dt_conf_get_float("plugins/darkroom/masks/brush/density"),1.0f);

      float radius1 = masks_border*masks_hardness*MIN(wd,ht);
      float radius2 = masks_border*MIN(wd,ht);

      float xpos, ypos;
      if(gui->posx == 0 && gui->posy == 0)
      {
        float zoom_x, zoom_y;
        DT_CTL_GET_GLOBAL(zoom_y, dev_zoom_y);
        DT_CTL_GET_GLOBAL(zoom_x, dev_zoom_x);
        xpos = (.5f+zoom_x)*wd;
        ypos = (.5f+zoom_y)*ht;
      }
      else
      {
        xpos = gui->posx;
        ypos = gui->posy;
      }

      cairo_save(cr);
      cairo_set_source_rgba(cr, .8, .8, .8, masks_density);
      cairo_arc(cr, xpos, ypos, radius1, 0, 2.0*M_PI);
      cairo_fill_preserve(cr);
      cairo_set_source_rgba(cr, .8, .8, .8, .8);
      cairo_stroke(cr);
      cairo_set_dash(cr, dashed, len, 0);
      cairo_arc(cr, xpos, ypos, radius2, 0, 2.0*M_PI);
      cairo_stroke(cr);
      cairo_restore(cr);
    }
    else
    {
      float masks_border, masks_hardness, masks_density;
      float radius, oldradius, opacity, oldopacity, pressure;
      int stroked = 1;

      cairo_save (cr);
      cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);
      cairo_set_line_cap (cr, CAIRO_LINE_CAP_ROUND);
      float linewidth = cairo_get_line_width(cr);
      masks_border = gui->guipoints_payload[0];
      masks_hardness = gui->guipoints_payload[1];
      masks_density = gui->guipoints_payload[2];
      pressure = gui->guipoints_payload[3];

      switch(gui->pressure_sensitivity)
      {
        case DT_MASKS_PRESSURE_HARDNESS_ABS:
          masks_hardness = MAX(0.05f, pressure);
          break;
        case DT_MASKS_PRESSURE_HARDNESS_REL:
          masks_hardness = MAX(0.05f, masks_hardness*pressure);
          break;
        case DT_MASKS_PRESSURE_OPACITY_ABS:
          masks_density = MAX(0.05f, pressure);
          break;
        case DT_MASKS_PRESSURE_OPACITY_REL:
          masks_density = MAX(0.05f, masks_density*pressure);
          break;
        case DT_MASKS_PRESSURE_BRUSHSIZE_REL:
          masks_border = MAX(0.005f, masks_border*pressure);
          break;
        default:
        case DT_MASKS_PRESSURE_OFF:
          //ignore pressure value
          break;
      }

      radius = oldradius = masks_border*masks_hardness*MIN(wd,ht);
      opacity = oldopacity = masks_density;

      cairo_set_line_width(cr, 2*radius);
      cairo_set_source_rgba(cr, .1, .1, .1, opacity);

      cairo_move_to(cr, gui->guipoints[0], gui->guipoints[1]);
      for (int i = 1; i < gui->guipoints_count; i++)
      {
        cairo_line_to(cr, gui->guipoints[i*2], gui->guipoints[i*2+1]);
        stroked = 0;
        masks_border = gui->guipoints_payload[i*4];
        masks_hardness = gui->guipoints_payload[i*4+1];
        masks_density = gui->guipoints_payload[i*4+2];
        pressure = gui->guipoints_payload[i*4+3];

        switch(gui->pressure_sensitivity)
        {
          case DT_MASKS_PRESSURE_HARDNESS_ABS:
            masks_hardness = MAX(0.05f, pressure);
            break;
          case DT_MASKS_PRESSURE_HARDNESS_REL:
            masks_hardness = MAX(0.05f, masks_hardness*pressure);
            break;
          case DT_MASKS_PRESSURE_OPACITY_ABS:
            masks_density = MAX(0.05f, pressure);
            break;
          case DT_MASKS_PRESSURE_OPACITY_REL:
            masks_density = MAX(0.05f, masks_density*pressure);
            break;
          case DT_MASKS_PRESSURE_BRUSHSIZE_REL:
            masks_border = MAX(0.005f, masks_border*pressure);
            break;
          default:
          case DT_MASKS_PRESSURE_OFF:
            //ignore pressure value
            break;
        }

        radius = masks_border*masks_hardness*MIN(wd,ht);
        opacity = masks_density;

        if(radius != oldradius || opacity != oldopacity)
        {
          cairo_stroke(cr);
          stroked = 1;
          cairo_set_line_width(cr, 2*radius);
          cairo_set_source_rgba(cr, .1, .1, .1, opacity);
          oldradius = radius;
          oldopacity = opacity;
          cairo_move_to(cr, gui->guipoints[i*2], gui->guipoints[i*2+1]);
        }
      }
      if (!stroked) cairo_stroke(cr);

      cairo_set_line_width(cr, linewidth);
      cairo_set_source_rgba(cr, .8, .8, .8, opacity);
      cairo_arc(cr, gui->guipoints[2*(gui->guipoints_count-1)], gui->guipoints[2*(gui->guipoints_count-1)+1], radius, 0, 2.0*M_PI);
      cairo_fill_preserve(cr);
      cairo_set_source_rgba(cr, .8, .8, .8, .8);
      cairo_stroke(cr);
      cairo_set_dash(cr, dashed, len, 0);
      cairo_arc(cr, gui->guipoints[2*(gui->guipoints_count-1)], gui->guipoints[2*(gui->guipoints_count-1)+1], masks_border*MIN(wd,ht), 0, 2.0*M_PI);
      cairo_stroke(cr);

      cairo_restore(cr);

    }
    return;
  }

  //draw path
  if (gpt->points_count > nb*3+2)
  {
    cairo_set_dash(cr, dashed, 0, 0);

    cairo_move_to(cr,gpt->points[nb*6]+dx,gpt->points[nb*6+1]+dy);
    int seg = 1, seg2 = 0;
    for (int i=nb*3; i<gpt->points_count; i++)
    {
      cairo_line_to(cr,gpt->points[i*2]+dx,gpt->points[i*2+1]+dy);
      //we decide to highlight the form segment by segment
      if (gpt->points[i*2+1] == gpt->points[seg*6+3] && gpt->points[i*2] == gpt->points[seg*6+2])
      {
        //this is the end of the last segment, so we have to draw it
        if ((gui->group_selected == index) && (gui->form_selected || gui->form_dragging || gui->seg_selected==seg2)) cairo_set_line_width(cr, 5.0/zoom_scale);
        else                                     cairo_set_line_width(cr, 3.0/zoom_scale);
        cairo_set_source_rgba(cr, .3, .3, .3, .8);
        cairo_stroke_preserve(cr);
        if ((gui->group_selected == index) && (gui->form_selected || gui->form_dragging || gui->seg_selected==seg2)) cairo_set_line_width(cr, 2.0/zoom_scale);
        else                                     cairo_set_line_width(cr, 1.0/zoom_scale);
        cairo_set_source_rgba(cr, .8, .8, .8, .8);
        cairo_stroke(cr);
        //and we update the segment number
        seg = (seg+1)%nb;
        seg2++;
        cairo_move_to(cr,gpt->points[i*2]+dx,gpt->points[i*2+1]+dy);
      }
    }
  }

  //draw corners
  float anchor_size;
  if (gui->group_selected == index && gpt->points_count > nb*3+2)
  {
    for(int k = 0; k < nb; k++)
    {
      if (k == gui->point_dragging || k == gui->point_selected)
      {
        anchor_size = 7.0f / zoom_scale;
      }
      else
      {
        anchor_size = 5.0f / zoom_scale;
      }
      cairo_set_source_rgba(cr, .8, .8, .8, .8);
      cairo_rectangle(cr,
                      gpt->points[k*6+2] - (anchor_size*0.5)+dx,
                      gpt->points[k*6+3] - (anchor_size*0.5)+dy,
                      anchor_size, anchor_size);
      cairo_fill_preserve(cr);

      if ((gui->group_selected == index) && (k == gui->point_dragging || k == gui->point_selected )) cairo_set_line_width(cr, 2.0/zoom_scale);
      else if ((gui->group_selected == index) && ((k == 0 || k == nb) && gui->creation && gui->creation_closing_form)) cairo_set_line_width(cr, 2.0/zoom_scale);
      else cairo_set_line_width(cr, 1.0/zoom_scale);
      cairo_set_source_rgba(cr, .3, .3, .3, .8);
      cairo_stroke(cr);
    }
  }

  //draw feathers
  if ((gui->group_selected == index) && gui->point_edited >= 0)
  {
    int k = gui->point_edited;
    //uncomment this part if you want to see "real" control points
    /*cairo_move_to(cr, gui->points[k*6+2]+dx,gui->points[k*6+3]+dy);
    cairo_line_to(cr, gui->points[k*6]+dx,gui->points[k*6+1]+dy);
    cairo_stroke(cr);
    cairo_move_to(cr, gui->points[k*6+2]+dx,gui->points[k*6+3]+dy);
    cairo_line_to(cr, gui->points[k*6+4]+dx,gui->points[k*6+5]+dy);
    cairo_stroke(cr);*/
    int ffx,ffy;
    _brush_ctrl2_to_feather(gpt->points[k*6+2]+dx,gpt->points[k*6+3]+dy,gpt->points[k*6+4]+dx,gpt->points[k*6+5]+dy,&ffx,&ffy,TRUE);
    cairo_move_to(cr, gpt->points[k*6+2]+dx,gpt->points[k*6+3]+dy);
    cairo_line_to(cr,ffx,ffy);
    cairo_set_line_width(cr, 1.5/zoom_scale);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    cairo_stroke_preserve(cr);
    cairo_set_line_width(cr, 0.75/zoom_scale);
    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    cairo_stroke(cr);

    if ((gui->group_selected == index) && (k == gui->feather_dragging || k == gui->feather_selected)) cairo_arc (cr, ffx,ffy, 3.0f / zoom_scale, 0, 2.0*M_PI);
    else cairo_arc (cr, ffx,ffy, 1.5f / zoom_scale, 0, 2.0*M_PI);
    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    cairo_fill_preserve(cr);

    cairo_set_line_width(cr, 1.0/zoom_scale);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    cairo_stroke(cr);
  }

  //draw border and corners
  if ((gui->group_selected == index) && gpt->border_count > nb*3+2)
  {


    cairo_move_to(cr,gpt->border[nb*6]+dx,gpt->border[nb*6+1]+dy);

    for (int i=nb*3+1; i<gpt->border_count; i++)
    {
      cairo_line_to(cr,gpt->border[i*2]+dx,gpt->border[i*2+1]+dy);
    }
    //we execute the drawing
    if (gui->border_selected) cairo_set_line_width(cr, 2.0/zoom_scale);
    else                                     cairo_set_line_width(cr, 1.0/zoom_scale);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    cairo_set_dash(cr, dashed, len, 0);
    cairo_stroke_preserve(cr);
    if (gui->border_selected) cairo_set_line_width(cr, 2.0/zoom_scale);
    else                                     cairo_set_line_width(cr, 1.0/zoom_scale);
    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    cairo_set_dash(cr, dashed, len, 4);
    cairo_stroke(cr);

#if 0
    //we draw the brush segment by segment
    for (int k=0; k<nb; k++)
    {
      //draw the point
      if (gui->point_border_selected == k)
      {
        anchor_size = 7.0f / zoom_scale;
      }
      else
      {
        anchor_size = 5.0f / zoom_scale;
      }
      cairo_set_source_rgba(cr, .8, .8, .8, .8);
      cairo_rectangle(cr,
                      gpt->border[k*6] - (anchor_size*0.5)+dx,
                      gpt->border[k*6+1] - (anchor_size*0.5)+dy,
                      anchor_size, anchor_size);
      cairo_fill_preserve(cr);

      if (gui->point_border_selected == k) cairo_set_line_width(cr, 2.0/zoom_scale);
      else cairo_set_line_width(cr, 1.0/zoom_scale);
      cairo_set_source_rgba(cr, .3, .3, .3, .8);
      cairo_set_dash(cr, dashed, 0, 0);
      cairo_stroke(cr);
    }
#endif
  }

  //draw the source if needed
  if (!gui->creation && gpt->source_count>nb*3+2)
  {
    //we draw the line between source and dest
    cairo_move_to(cr,gpt->source[2]+dxs,gpt->source[3]+dys);
    cairo_line_to(cr,gpt->points[2]+dx,gpt->points[3]+dy);
    cairo_set_dash(cr, dashed, 0, 0);
    if ((gui->group_selected == index) && (gui->form_selected || gui->form_dragging)) cairo_set_line_width(cr, 2.5/zoom_scale);
    else                                     cairo_set_line_width(cr, 1.5/zoom_scale);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    cairo_stroke_preserve(cr);
    if ((gui->group_selected == index) && (gui->form_selected || gui->form_dragging)) cairo_set_line_width(cr, 1.0/zoom_scale);
    else                                     cairo_set_line_width(cr, 0.5/zoom_scale);
    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    cairo_stroke(cr);

    //we draw the source
    cairo_set_dash(cr, dashed, 0, 0);
    if ((gui->group_selected == index) && (gui->form_selected || gui->form_dragging)) cairo_set_line_width(cr, 2.5/zoom_scale);
    else                                     cairo_set_line_width(cr, 1.5/zoom_scale);
    cairo_set_source_rgba(cr, .3, .3, .3, .8);
    cairo_move_to(cr,gpt->source[nb*6]+dxs,gpt->source[nb*6+1]+dys);
    for (int i=nb*3; i<gpt->source_count; i++) cairo_line_to(cr,gpt->source[i*2]+dxs,gpt->source[i*2+1]+dys);
    cairo_line_to(cr,gpt->source[nb*6]+dxs,gpt->source[nb*6+1]+dys);
    cairo_stroke_preserve(cr);
    if ((gui->group_selected == index) && (gui->form_selected || gui->form_dragging)) cairo_set_line_width(cr, 1.0/zoom_scale);
    else                                     cairo_set_line_width(cr, 0.5/zoom_scale);
    cairo_set_source_rgba(cr, .8, .8, .8, .8);
    cairo_stroke(cr);
  }
}

static int dt_brush_get_source_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form, int *width, int *height, int *posx, int *posy)
{
  if (!module) return 0;
  //we get buffers for all points
  float *points, *border;
  int points_count,border_count;
  if (!_brush_get_points_border(module->dev,form,module->priority,piece->pipe,&points,&points_count,&border,&border_count,NULL,NULL,1)) return 0;

  //now we want to find the area, so we search min/max points
  float xmin, xmax, ymin, ymax;
  xmin = ymin = FLT_MAX;
  xmax = ymax = FLT_MIN;
  int nb_corner = g_list_length(form->points);
  for (int i=nb_corner*3; i < border_count; i++)
  {
    //we look at the borders
    float xx = border[i*2];
    float yy = border[i*2+1];
    xmin = fminf(xx,xmin);
    xmax = fmaxf(xx,xmax);
    ymin = fminf(yy,ymin);
    ymax = fmaxf(yy,ymax);
  }
  for (int i=nb_corner*3; i < points_count; i++)
  {
    //we look at the brush too
    float xx = points[i*2];
    float yy = points[i*2+1];
    xmin = fminf(xx,xmin);
    xmax = fmaxf(xx,xmax);
    ymin = fminf(yy,ymin);
    ymax = fmaxf(yy,ymax);
  }

  free(points);
  free(border);
  *height = ymax-ymin+4;
  *width = xmax-xmin+4;
  *posx = xmin-2;
  *posy = ymin-2;
  return 1;
}

static int dt_brush_get_area(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form, int *width, int *height, int *posx, int *posy)
{
  if (!module) return 0;
  //we get buffers for all points
  float *points, *border;
  int points_count,border_count;
  if (!_brush_get_points_border(module->dev,form,module->priority,piece->pipe,&points,&points_count,&border,&border_count,NULL,NULL,0)) return 0;

  //now we want to find the area, so we search min/max points
  float xmin, xmax, ymin, ymax;
  xmin = ymin = FLT_MAX;
  xmax = ymax = FLT_MIN;
  int nb_corner = g_list_length(form->points);
  for (int i=nb_corner*3; i < border_count; i++)
  {
    //we look at the borders
    float xx = border[i*2];
    float yy = border[i*2+1];
    xmin = fminf(xx,xmin);
    xmax = fmaxf(xx,xmax);
    ymin = fminf(yy,ymin);
    ymax = fmaxf(yy,ymax);
  }
  for (int i=nb_corner*3; i < points_count; i++)
  {
    //we look at the brush too
    float xx = points[i*2];
    float yy = points[i*2+1];
    xmin = fminf(xx,xmin);
    xmax = fmaxf(xx,xmax);
    ymin = fminf(yy,ymin);
    ymax = fmaxf(yy,ymax);
  }

  free(points);
  free(border);

  *height = ymax-ymin+4;
  *width = xmax-xmin+4;
  *posx = xmin-2;
  *posy = ymin-2;
  return 1;
}

/** we write a falloff segment */
static void _brush_falloff(float **buffer, int *p0, int *p1, int posx, int posy, int bw, float hardness, float density)
{
  //segment length
  const int l = sqrt((p1[0]-p0[0])*(p1[0]-p0[0])+(p1[1]-p0[1])*(p1[1]-p0[1]))+1;
  const int solid = (int)l*hardness;
  const int soft = l - solid;

  const float lx = p1[0]-p0[0];
  const float ly = p1[1]-p0[1];

  for (int i=0 ; i<l; i++)
  {
    //position
    const int x = (int)((float)i*lx/(float)l) + p0[0] - posx;
    const int y = (int)((float)i*ly/(float)l) + p0[1] - posy;
    const float op = density * ((i <= solid) ? 1.0f : 1.0-(float)(i - solid)/(float)soft);
    (*buffer)[y*bw+x] = fmaxf((*buffer)[y*bw+x],op);
    if (x > 0) (*buffer)[y*bw+x-1] = fmaxf((*buffer)[y*bw+x-1],op); //this one is to avoid gap due to int rounding
    if (y > 0) (*buffer)[(y-1)*bw+x] = fmaxf((*buffer)[(y-1)*bw+x],op); //this one is to avoid gap due to int rounding
  }
}

static int dt_brush_get_mask(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form, float **buffer, int *width, int *height, int *posx, int *posy)
{
  if (!module) return 0;
  double start = dt_get_wtime();
  double start2;

  //we get buffers for all points
  float *points, *border, *payload;
  int points_count,border_count,payload_count;
  if (!_brush_get_points_border(module->dev,form,module->priority,piece->pipe,&points,&points_count,&border,&border_count,&payload,&payload_count,0)) return 0;

  if (darktable.unmuted & DT_DEBUG_PERF) dt_print(DT_DEBUG_MASKS, "[masks %s] brush points took %0.04f sec\n", form->name, dt_get_wtime()-start);
  start = start2 = dt_get_wtime();

  //now we want to find the area, so we search min/max points
  float xmin, xmax, ymin, ymax;
  xmin = ymin = FLT_MAX;
  xmax = ymax = FLT_MIN;
  int nb_corner = g_list_length(form->points);
  for (int i=nb_corner*3; i < border_count; i++)
  {
    //we look at the borders
    float xx = border[i*2];
    float yy = border[i*2+1];
    xmin = fminf(xx,xmin);
    xmax = fmaxf(xx,xmax);
    ymin = fminf(yy,ymin);
    ymax = fmaxf(yy,ymax);
  }
  for (int i=nb_corner*3; i < points_count; i++)
  {
    //we look at the brush too
    float xx = points[i*2];
    float yy = points[i*2+1];
    xmin = fminf(xx,xmin);
    xmax = fmaxf(xx,xmax);
    ymin = fminf(yy,ymin);
    ymax = fmaxf(yy,ymax);
  }

  *height = ymax-ymin+4;
  *width = xmax-xmin+4;
  *posx = xmin-2;
  *posy = ymin-2;

  if (darktable.unmuted & DT_DEBUG_PERF) dt_print(DT_DEBUG_MASKS, "[masks %s] brush_fill min max took %0.04f sec\n", form->name, dt_get_wtime()-start2);
  start2 = dt_get_wtime();

  //we allocate the buffer
  *buffer = malloc((*width)*(*height)*sizeof(float));
  memset(*buffer,0,(*width)*(*height)*sizeof(float));

  //now we fill the falloff
  int p0[2], p1[2];

  for (int i=nb_corner*3; i<border_count; i++)
  {
    p0[0] = points[i*2];
    p0[1] = points[i*2+1];
    p1[0] = border[i*2];
    p1[1] = border[i*2+1];

    _brush_falloff(buffer,p0,p1,*posx,*posy,*width,payload[i*2],payload[i*2+1]);
  }

  free(points);
  free(border);
  free(payload);

  if (darktable.unmuted & DT_DEBUG_PERF) dt_print(DT_DEBUG_MASKS, "[masks %s] brush fill buffer took %0.04f sec\n", form->name, dt_get_wtime()-start);

  return 1;
}

/** we write a falloff segment respecting limits of buffer */
static inline void _brush_falloff_roi(float **buffer, int *p0, int *p1, int bw, int bh, float hardness, float density)
{
  //segment length (increase by 1 to avoid division-by-zero special case handling)
  const int l = sqrt((p1[0]-p0[0])*(p1[0]-p0[0])+(p1[1]-p0[1])*(p1[1]-p0[1])) + 1;
  const int solid = hardness*l;

  const float lx = (float)(p1[0]-p0[0])/(float)l;
  const float ly = (float)(p1[1]-p0[1])/(float)l;

  const int dx = lx <= 0 ? -1 : 1;
  const int dy = ly <= 0 ? -1 : 1;
  const int dpx = dx;
  const int dpy = dy*bw;
  
  float fx = p0[0];
  float fy = p0[1];

  float op = density;
  float dop = density/(float)(l - solid);

  for (int i=0 ; i<l; i++)
  {
    const int x = fx;
    const int y = fy;

    fx += lx;
    fy += ly;
    if (i > solid) op -= dop;

    if (x < 0 || x >= bw || y < 0 || y >= bh) continue;

    float *buf = *buffer + y*bw + x;

    *buf = fmaxf(*buf, op);
    if (x+dx >= 0 && x+dx < bw) buf[dpx] = fmaxf(buf[dpx], op);   //this one is to avoid gaps due to int rounding
    if (y+dy >= 0 && y+dy < bh) buf[dpy] = fmaxf(buf[dpy], op);   //this one is to avoid gaps due to int rounding
  }
}

static int dt_brush_get_mask_roi(dt_iop_module_t *module, dt_dev_pixelpipe_iop_t *piece, dt_masks_form_t *form, const dt_iop_roi_t *roi, float **buffer)
{
  if (!module) return 0;
  double start = dt_get_wtime();
  double start2;

  const int px = roi->x;
  const int py = roi->y;
  const int width = roi->width;
  const int height = roi->height;
  const float scale = roi->scale;

  //we get buffers for all points
  float *points, *border, *payload;

  int points_count, border_count, payload_count;
  if (!_brush_get_points_border(module->dev,form,module->priority,piece->pipe,&points,&points_count,&border,&border_count,&payload,&payload_count,0)) return 0;

  if (darktable.unmuted & DT_DEBUG_PERF) dt_print(DT_DEBUG_MASKS, "[masks %s] brush points took %0.04f sec\n", form->name, dt_get_wtime()-start);
  start = start2 = dt_get_wtime();

  //we allocate the output buffer
  *buffer = malloc(width*height*sizeof(float));
  if (*buffer == NULL)
  {
    free(points);
    free(border);
    return 0;
  }
  memset(*buffer,0,width*height*sizeof(float));

  int nb_corner = g_list_length(form->points);

  //we shift and scale down brush and border
  for (int i=nb_corner*3; i < border_count; i++)
  {
    float xx = border[2*i];
    float yy = border[2*i+1];
    border[2*i] = xx * scale - px;
    border[2*i+1] = yy * scale - py;
  }

  for (int i=nb_corner*3; i < points_count; i++)
  {
    float xx = points[2*i];
    float yy = points[2*i+1];
    points[2*i] = xx * scale - px;
    points[2*i+1] = yy * scale - py;
  }

  //now we want to find the area, so we search min/max points
  float xmin, xmax, ymin, ymax;
  xmin = ymin = FLT_MAX;
  xmax = ymax = FLT_MIN;

  for (int i=nb_corner*3; i < border_count; i++)
  {
    //we look at the borders
    float xx = border[i*2];
    float yy = border[i*2+1];
    xmin = fminf(xx,xmin);
    xmax = fmaxf(xx,xmax);
    ymin = fminf(yy,ymin);
    ymax = fmaxf(yy,ymax);
  }
  for (int i=nb_corner*3; i < points_count; i++)
  {
    //we look at the brush too
    float xx = points[i*2];
    float yy = points[i*2+1];
    xmin = fminf(xx,xmin);
    xmax = fmaxf(xx,xmax);
    ymin = fminf(yy,ymin);
    ymax = fmaxf(yy,ymax);
  }

  if (darktable.unmuted & DT_DEBUG_PERF) dt_print(DT_DEBUG_MASKS, "[masks %s] brush_fill min max took %0.04f sec\n", form->name, dt_get_wtime()-start2);
  start2 = dt_get_wtime();

  //check if the path completely lies outside of roi -> we're done/mask remains empty
  if(xmax < 0 || ymax < 0 || xmin >= width || ymin >= height)
  {
    free(points);
    free(border);
    free(payload);
    return 1;
  }

  //now we fill the falloff
  int p0[2], p1[2];
  for (int i=nb_corner*3; i<border_count; i++)
  {
    p0[0] = points[i*2];
    p0[1] = points[i*2+1];
    p1[0] = border[i*2];
    p1[1] = border[i*2+1];

    if(MAX(p0[0], p1[0]) < 0 || MIN(p0[0], p1[0]) >= width || MAX(p0[1], p1[1]) < 0 || MIN(p0[1], p1[1]) >= height) continue;

    _brush_falloff_roi(buffer, p0, p1, width, height, payload[i*2], payload[i*2+1]);
  }

  free(points);
  free(border);
  free(payload);

  if (darktable.unmuted & DT_DEBUG_PERF) dt_print(DT_DEBUG_MASKS, "[masks %s] brush fill buffer took %0.04f sec\n", form->name, dt_get_wtime()-start);

  return 1;
}

// modelines: These editor modelines have been set for all relevant files by tools/update_modelines.sh
// vim: shiftwidth=2 expandtab tabstop=2 cindent
// kate: tab-indents: off; indent-width 2; replace-tabs on; indent-mode cstyle; remove-trailing-space on;
