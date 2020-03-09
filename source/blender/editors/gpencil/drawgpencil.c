/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2008, Blender Foundation
 * This is a new part of Blender
 */

/** \file
 * \ingroup edgpencil
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <math.h>
#include <float.h>

#include "MEM_guardedalloc.h"

#include "BLI_sys_types.h"

#include "BLI_math.h"
#include "BLI_utildefines.h"
#include "BLI_polyfill_2d.h"

#include "BLF_api.h"
#include "BLT_translation.h"

#include "DNA_brush_types.h"
#include "DNA_gpencil_types.h"
#include "DNA_scene_types.h"
#include "DNA_screen_types.h"
#include "DNA_space_types.h"
#include "DNA_view3d_types.h"
#include "DNA_userdef_types.h"
#include "DNA_object_types.h"

#include "BKE_context.h"
#include "BKE_brush.h"
#include "BKE_global.h"
#include "BKE_material.h"
#include "BKE_paint.h"
#include "BKE_gpencil.h"
#include "BKE_image.h"

#include "DEG_depsgraph.h"

#include "WM_api.h"

#include "BIF_glutil.h"

#include "GPU_immediate.h"
#include "GPU_state.h"

#include "ED_gpencil.h"
#include "ED_screen.h"
#include "ED_view3d.h"
#include "ED_space_api.h"

#include "UI_interface_icons.h"
#include "UI_resources.h"

#include "IMB_imbuf_types.h"

#include "gpencil_intern.h"

/* ************************************************** */
/* GREASE PENCIL DRAWING */

/* ----- General Defines ------ */
/* flags for sflag */
typedef enum eDrawStrokeFlags {
  /** don't draw status info */
  GP_DRAWDATA_NOSTATUS = (1 << 0),
  /** only draw 3d-strokes */
  GP_DRAWDATA_ONLY3D = (1 << 1),
  /** only draw 'canvas' strokes */
  GP_DRAWDATA_ONLYV2D = (1 << 2),
  /** only draw 'image' strokes */
  GP_DRAWDATA_ONLYI2D = (1 << 3),
  /** special hack for drawing strokes in Image Editor (weird coordinates) */
  GP_DRAWDATA_IEDITHACK = (1 << 4),
  /** don't draw xray in 3D view (which is default) */
  GP_DRAWDATA_NO_XRAY = (1 << 5),
  /** no onionskins should be drawn (for animation playback) */
  GP_DRAWDATA_NO_ONIONS = (1 << 6),
  /** draw strokes as "volumetric" circular billboards */
  GP_DRAWDATA_VOLUMETRIC = (1 << 7),
  /** fill insides/bounded-regions of strokes */
  GP_DRAWDATA_FILL = (1 << 8),
} eDrawStrokeFlags;

/* thickness above which we should use special drawing */
#if 0
#  define GP_DRAWTHICKNESS_SPECIAL 3
#endif

/* conversion utility (float --> normalized unsigned byte) */
#define F2UB(x) (uchar)(255.0f * x)

/* ----- Tool Buffer Drawing ------ */
/* helper functions to set color of buffer point */

static void gp_set_point_uniform_color(const bGPDspoint *pt, const float ink[4])
{
  float alpha = ink[3] * pt->strength;
  CLAMP(alpha, GPENCIL_STRENGTH_MIN, 1.0f);
  immUniformColor3fvAlpha(ink, alpha);
}

static void gp_set_point_varying_color(const bGPDspoint *pt,
                                       const float ink[4],
                                       uint attr_id,
                                       bool fix_strength)
{
  float alpha = ink[3] * pt->strength;
  if ((fix_strength) && (alpha >= 0.1f)) {
    alpha = 1.0f;
  }
  CLAMP(alpha, GPENCIL_STRENGTH_MIN, 1.0f);
  immAttr4ub(attr_id, F2UB(ink[0]), F2UB(ink[1]), F2UB(ink[2]), F2UB(alpha));
}

/* --------- 2D Stroke Drawing Helpers --------- */
/* change in parameter list */
static void gp_calc_2d_stroke_fxy(
    const float pt[3], short sflag, int offsx, int offsy, int winx, int winy, float r_co[2])
{
  if (sflag & GP_STROKE_2DSPACE) {
    r_co[0] = pt[0];
    r_co[1] = pt[1];
  }
  else if (sflag & GP_STROKE_2DIMAGE) {
    const float x = (float)((pt[0] * winx) + offsx);
    const float y = (float)((pt[1] * winy) + offsy);

    r_co[0] = x;
    r_co[1] = y;
  }
  else {
    const float x = (float)(pt[0] / 100 * winx) + offsx;
    const float y = (float)(pt[1] / 100 * winy) + offsy;

    r_co[0] = x;
    r_co[1] = y;
  }
}
/* ----------- Volumetric Strokes --------------- */

/* draw a 2D strokes in "volumetric" style */
static void gp_draw_stroke_volumetric_2d(const bGPDspoint *points,
                                         int totpoints,
                                         short thickness,
                                         short UNUSED(dflag),
                                         short sflag,
                                         int offsx,
                                         int offsy,
                                         int winx,
                                         int winy,
                                         const float diff_mat[4][4],
                                         const float ink[4])
{
  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
  uint size = GPU_vertformat_attr_add(format, "size", GPU_COMP_F32, 1, GPU_FETCH_FLOAT);
  uint color = GPU_vertformat_attr_add(
      format, "color", GPU_COMP_U8, 4, GPU_FETCH_INT_TO_FLOAT_UNIT);

  immBindBuiltinProgram(GPU_SHADER_3D_POINT_VARYING_SIZE_VARYING_COLOR);
  GPU_program_point_size(true);
  immBegin(GPU_PRIM_POINTS, totpoints);

  const bGPDspoint *pt = points;
  for (int i = 0; i < totpoints; i++, pt++) {
    /* transform position to 2D */
    float co[2];
    float fpt[3];

    mul_v3_m4v3(fpt, diff_mat, &pt->x);
    gp_calc_2d_stroke_fxy(fpt, sflag, offsx, offsy, winx, winy, co);

    gp_set_point_varying_color(pt, ink, color, false);
    immAttr1f(size, pt->pressure * thickness); /* TODO: scale based on view transform */
    immVertex2f(pos, co[0], co[1]);
  }

  immEnd();
  immUnbindProgram();
  GPU_program_point_size(false);
}

/* draw a 3D stroke in "volumetric" style */
static void gp_draw_stroke_volumetric_3d(const bGPDspoint *points,
                                         int totpoints,
                                         short thickness,
                                         const float ink[4])
{
  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
  uint size = GPU_vertformat_attr_add(format, "size", GPU_COMP_F32, 1, GPU_FETCH_FLOAT);
  uint color = GPU_vertformat_attr_add(
      format, "color", GPU_COMP_U8, 4, GPU_FETCH_INT_TO_FLOAT_UNIT);

  immBindBuiltinProgram(GPU_SHADER_3D_POINT_VARYING_SIZE_VARYING_COLOR);
  GPU_program_point_size(true);
  immBegin(GPU_PRIM_POINTS, totpoints);

  const bGPDspoint *pt = points;
  for (int i = 0; i < totpoints && pt; i++, pt++) {
    gp_set_point_varying_color(pt, ink, color, false);
    /* TODO: scale based on view transform */
    immAttr1f(size, pt->pressure * thickness);
    /* we can adjust size in vertex shader based on view/projection! */
    immVertex3fv(pos, &pt->x);
  }

  immEnd();
  immUnbindProgram();
  GPU_program_point_size(false);
}

/* --------------- Stroke Fills ----------------- */
/* add a new fill point and texture coordinates to vertex buffer */
static void gp_add_filldata_tobuffer(const bGPDspoint *pt,
                                     uint pos,
                                     uint texcoord,
                                     short flag,
                                     int offsx,
                                     int offsy,
                                     int winx,
                                     int winy,
                                     const float diff_mat[4][4])
{
  float fpt[3];
  float co[2];

  mul_v3_m4v3(fpt, diff_mat, &pt->x);
  /* if 2d, need conversion */
  if (!(flag & GP_STROKE_3DSPACE)) {
    gp_calc_2d_stroke_fxy(fpt, flag, offsx, offsy, winx, winy, co);
    copy_v2_v2(fpt, co);
    fpt[2] = 0.0f; /* 2d always is z=0.0f */
  }

  immAttr2f(texcoord, pt->uv_fill[0], pt->uv_fill[1]); /* texture coordinates */
  immVertex3fv(pos, fpt);                              /* position */
}

/* draw fills for shapes */
static void gp_draw_stroke_fill(bGPdata *gpd,
                                bGPDstroke *gps,
                                int offsx,
                                int offsy,
                                int winx,
                                int winy,
                                const float diff_mat[4][4],
                                const float color[4])
{
  BLI_assert(gps->totpoints >= 3);
  BLI_assert(gps->tot_triangles >= 1);
  const bool use_mat = (gpd->mat != NULL);

  Material *ma = (use_mat) ? gpd->mat[gps->mat_nr] : BKE_material_default_gpencil();
  MaterialGPencilStyle *gp_style = (ma) ? ma->gp_style : NULL;

  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);
  uint texcoord = GPU_vertformat_attr_add(format, "texCoord", GPU_COMP_F32, 2, GPU_FETCH_FLOAT);
  immBindBuiltinProgram(GPU_SHADER_GPENCIL_FILL);

  immUniformColor4fv(color);
  immUniform4fv("color2", gp_style->mix_rgba);
  immUniform1i("fill_type", gp_style->fill_style);
  immUniform1f("mix_factor", gp_style->mix_factor);

  immUniform1f("texture_angle", gp_style->texture_angle);
  immUniform2fv("texture_scale", gp_style->texture_scale);
  immUniform2fv("texture_offset", gp_style->texture_offset);
  immUniform1f("texture_opacity", gp_style->texture_opacity);
  immUniform1i("t_mix", (gp_style->flag & GP_MATERIAL_FILL_TEX_MIX) != 0);
  immUniform1i("t_flip", (gp_style->flag & GP_MATERIAL_FLIP_FILL) != 0);

  /* Draw all triangles for filling the polygon (cache must be calculated before) */
  immBegin(GPU_PRIM_TRIS, gps->tot_triangles * 3);
  /* TODO: use batch instead of immediate mode, to share vertices */

  const bGPDtriangle *stroke_triangle = gps->triangles;
  for (int i = 0; i < gps->tot_triangles; i++, stroke_triangle++) {
    for (int j = 0; j < 3; j++) {
      gp_add_filldata_tobuffer(&gps->points[stroke_triangle->verts[j]],
                               pos,
                               texcoord,
                               gps->flag,
                               offsx,
                               offsy,
                               winx,
                               winy,
                               diff_mat);
    }
  }

  immEnd();
  immUnbindProgram();
}

/* ----- Existing Strokes Drawing (3D and Point) ------ */

/* draw a given stroke - just a single dot (only one point) */
static void gp_draw_stroke_point(const bGPDspoint *points,
                                 short thickness,
                                 short UNUSED(dflag),
                                 short sflag,
                                 int offsx,
                                 int offsy,
                                 int winx,
                                 int winy,
                                 const float diff_mat[4][4],
                                 const float ink[4])
{
  const bGPDspoint *pt = points;

  /* get final position using parent matrix */
  float fpt[3];
  mul_v3_m4v3(fpt, diff_mat, &pt->x);

  GPUVertFormat *format = immVertexFormat();
  uint pos = GPU_vertformat_attr_add(format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT);

  if (sflag & GP_STROKE_3DSPACE) {
    immBindBuiltinProgram(GPU_SHADER_3D_POINT_UNIFORM_SIZE_UNIFORM_COLOR_AA);
  }
  else {
    immBindBuiltinProgram(GPU_SHADER_2D_POINT_UNIFORM_SIZE_UNIFORM_COLOR_AA);

    /* get 2D coordinates of point */
    float co[3] = {0.0f};
    gp_calc_2d_stroke_fxy(fpt, sflag, offsx, offsy, winx, winy, co);
    copy_v3_v3(fpt, co);
  }

  gp_set_point_uniform_color(pt, ink);
  /* set point thickness (since there's only one of these) */
  immUniform1f("size", (float)(thickness + 2) * pt->pressure);

  immBegin(GPU_PRIM_POINTS, 1);
  immVertex3fv(pos, fpt);
  immEnd();

  immUnbindProgram();
}

/* draw a given stroke in 3d (i.e. in 3d-space) */
static void gp_draw_stroke_3d(tGPDdraw *tgpw, short thickness, const float ink[4], bool cyclic)
{
  bGPDspoint *points = tgpw->gps->points;
  int totpoints = tgpw->gps->totpoints;

  const float viewport[2] = {(float)tgpw->winx, (float)tgpw->winy};
  float curpressure = points[0].pressure;
  float fpt[3];

  /* if cyclic needs more vertex */
  int cyclic_add = (cyclic) ? 1 : 0;

  GPUVertFormat *format = immVertexFormat();
  const struct {
    uint pos, color, thickness;
  } attr_id = {
      .pos = GPU_vertformat_attr_add(format, "pos", GPU_COMP_F32, 3, GPU_FETCH_FLOAT),
      .color = GPU_vertformat_attr_add(
          format, "color", GPU_COMP_U8, 4, GPU_FETCH_INT_TO_FLOAT_UNIT),
      .thickness = GPU_vertformat_attr_add(format, "thickness", GPU_COMP_F32, 1, GPU_FETCH_FLOAT),
  };

  immBindBuiltinProgram(GPU_SHADER_GPENCIL_STROKE);
  immUniform2fv("Viewport", viewport);
  immUniform1f("pixsize", tgpw->rv3d->pixsize);
  float obj_scale = tgpw->ob ?
                        (tgpw->ob->scale[0] + tgpw->ob->scale[1] + tgpw->ob->scale[2]) / 3.0f :
                        1.0f;

  immUniform1f("objscale", obj_scale);
  int keep_size = (int)((tgpw->gpd) && (tgpw->gpd->flag & GP_DATA_STROKE_KEEPTHICKNESS));
  immUniform1i("keep_size", keep_size);
  immUniform1f("pixfactor", tgpw->gpd->pixfactor);
  /* xray mode always to 3D space to avoid wrong zdepth calculation (T60051) */
  immUniform1i("xraymode", GP_XRAY_3DSPACE);
  immUniform1i("caps_start", (int)tgpw->gps->caps[0]);
  immUniform1i("caps_end", (int)tgpw->gps->caps[1]);
  immUniform1i("fill_stroke", (int)tgpw->is_fill_stroke);

  /* draw stroke curve */
  GPU_line_width(max_ff(curpressure * thickness, 1.0f));
  immBeginAtMost(GPU_PRIM_LINE_STRIP_ADJ, totpoints + cyclic_add + 2);
  const bGPDspoint *pt = points;

  for (int i = 0; i < totpoints; i++, pt++) {
    /* first point for adjacency (not drawn) */
    if (i == 0) {
      gp_set_point_varying_color(points, ink, attr_id.color, (bool)tgpw->is_fill_stroke);

      if ((cyclic) && (totpoints > 2)) {
        immAttr1f(attr_id.thickness, max_ff((points + totpoints - 1)->pressure * thickness, 1.0f));
        mul_v3_m4v3(fpt, tgpw->diff_mat, &(points + totpoints - 1)->x);
      }
      else {
        immAttr1f(attr_id.thickness, max_ff((points + 1)->pressure * thickness, 1.0f));
        mul_v3_m4v3(fpt, tgpw->diff_mat, &(points + 1)->x);
      }
      immVertex3fv(attr_id.pos, fpt);
    }
    /* set point */
    gp_set_point_varying_color(pt, ink, attr_id.color, (bool)tgpw->is_fill_stroke);
    immAttr1f(attr_id.thickness, max_ff(pt->pressure * thickness, 1.0f));
    mul_v3_m4v3(fpt, tgpw->diff_mat, &pt->x);
    immVertex3fv(attr_id.pos, fpt);
  }

  if (cyclic && totpoints > 2) {
    /* draw line to first point to complete the cycle */
    immAttr1f(attr_id.thickness, max_ff(points->pressure * thickness, 1.0f));
    mul_v3_m4v3(fpt, tgpw->diff_mat, &points->x);
    immVertex3fv(attr_id.pos, fpt);

    /* now add adjacency point (not drawn) */
    immAttr1f(attr_id.thickness, max_ff((points + 1)->pressure * thickness, 1.0f));
    mul_v3_m4v3(fpt, tgpw->diff_mat, &(points + 1)->x);
    immVertex3fv(attr_id.pos, fpt);
  }
  /* last adjacency point (not drawn) */
  else {
    gp_set_point_varying_color(
        points + totpoints - 2, ink, attr_id.color, (bool)tgpw->is_fill_stroke);

    immAttr1f(attr_id.thickness, max_ff((points + totpoints - 2)->pressure * thickness, 1.0f));
    mul_v3_m4v3(fpt, tgpw->diff_mat, &(points + totpoints - 2)->x);
    immVertex3fv(attr_id.pos, fpt);
  }

  immEnd();
  immUnbindProgram();
}

/* ----- Fancy 2D-Stroke Drawing ------ */

/* draw a given stroke in 2d */
static void gp_draw_stroke_2d(const bGPDspoint *points,
                              int totpoints,
                              short thickness_s,
                              short dflag,
                              short sflag,
                              bool UNUSED(debug),
                              int offsx,
                              int offsy,
                              int winx,
                              int winy,
                              const float diff_mat[4][4],
                              const float ink[4])
{
  /* otherwise thickness is twice that of the 3D view */
  float thickness = (float)thickness_s * 0.5f;

  /* strokes in Image Editor need a scale factor, since units there are not pixels! */
  float scalefac = 1.0f;
  if ((dflag & GP_DRAWDATA_IEDITHACK) && (dflag & GP_DRAWDATA_ONLYV2D)) {
    scalefac = 0.001f;
  }

  /* TODO: fancy++ with the magic of shaders */

  /* tessellation code - draw stroke as series of connected quads (triangle strips in fact)
   * with connection edges rotated to minimize shrinking artifacts, and rounded endcaps.
   */
  {
    const bGPDspoint *pt1, *pt2;
    float s0[2], s1[2]; /* segment 'center' points */
    float pm[2];        /* normal from previous segment. */
    int i;
    float fpt[3];

    GPUVertFormat *format = immVertexFormat();
    const struct {
      uint pos, color;
    } attr_id = {
        .pos = GPU_vertformat_attr_add(format, "pos", GPU_COMP_F32, 2, GPU_FETCH_FLOAT),
        .color = GPU_vertformat_attr_add(
            format, "color", GPU_COMP_U8, 4, GPU_FETCH_INT_TO_FLOAT_UNIT),
    };

    immBindBuiltinProgram(GPU_SHADER_2D_FLAT_COLOR);
    immBegin(GPU_PRIM_TRI_STRIP, totpoints * 2 + 4);

    /* get x and y coordinates from first point */
    mul_v3_m4v3(fpt, diff_mat, &points->x);
    gp_calc_2d_stroke_fxy(fpt, sflag, offsx, offsy, winx, winy, s0);

    for (i = 0, pt1 = points, pt2 = points + 1; i < (totpoints - 1); i++, pt1++, pt2++) {
      float t0[2], t1[2]; /* tessellated coordinates */
      float m1[2], m2[2]; /* gradient and normal */
      float mt[2], sc[2]; /* gradient for thickness, point for end-cap */
      float pthick;       /* thickness at segment point */

      /* Get x and y coordinates from point2
       * (point1 has already been computed in previous iteration). */
      mul_v3_m4v3(fpt, diff_mat, &pt2->x);
      gp_calc_2d_stroke_fxy(fpt, sflag, offsx, offsy, winx, winy, s1);

      /* calculate gradient and normal - 'angle'=(ny/nx) */
      m1[1] = s1[1] - s0[1];
      m1[0] = s1[0] - s0[0];
      normalize_v2(m1);
      m2[1] = -m1[0];
      m2[0] = m1[1];

      /* always use pressure from first point here */
      pthick = (pt1->pressure * thickness * scalefac);

      /* color of point */
      gp_set_point_varying_color(pt1, ink, attr_id.color, false);

      /* if the first segment, start of segment is segment's normal */
      if (i == 0) {
        /* draw start cap first
         * - make points slightly closer to center (about halfway across)
         */
        mt[0] = m2[0] * pthick * 0.5f;
        mt[1] = m2[1] * pthick * 0.5f;
        sc[0] = s0[0] - (m1[0] * pthick * 0.75f);
        sc[1] = s0[1] - (m1[1] * pthick * 0.75f);

        t0[0] = sc[0] - mt[0];
        t0[1] = sc[1] - mt[1];
        t1[0] = sc[0] + mt[0];
        t1[1] = sc[1] + mt[1];

        /* First two points of cap. */
        immVertex2fv(attr_id.pos, t0);
        immVertex2fv(attr_id.pos, t1);

        /* calculate points for start of segment */
        mt[0] = m2[0] * pthick;
        mt[1] = m2[1] * pthick;

        t0[0] = s0[0] - mt[0];
        t0[1] = s0[1] - mt[1];
        t1[0] = s0[0] + mt[0];
        t1[1] = s0[1] + mt[1];

        /* Last two points of start cap (and first two points of first segment). */
        immVertex2fv(attr_id.pos, t0);
        immVertex2fv(attr_id.pos, t1);
      }
      /* if not the first segment, use bisector of angle between segments */
      else {
        float mb[2];        /* bisector normal */
        float athick, dfac; /* actual thickness, difference between thicknesses */

        /* calculate gradient of bisector (as average of normals) */
        mb[0] = (pm[0] + m2[0]) / 2;
        mb[1] = (pm[1] + m2[1]) / 2;
        normalize_v2(mb);

        /* calculate gradient to apply
         * - as basis, use just pthick * bisector gradient
         * - if cross-section not as thick as it should be, add extra padding to fix it
         */
        mt[0] = mb[0] * pthick;
        mt[1] = mb[1] * pthick;
        athick = len_v2(mt);
        dfac = pthick - (athick * 2);

        if (((athick * 2.0f) < pthick) && (IS_EQF(athick, pthick) == 0)) {
          mt[0] += (mb[0] * dfac);
          mt[1] += (mb[1] * dfac);
        }

        /* calculate points for start of segment */
        t0[0] = s0[0] - mt[0];
        t0[1] = s0[1] - mt[1];
        t1[0] = s0[0] + mt[0];
        t1[1] = s0[1] + mt[1];

        /* Last two points of previous segment, and first two points of current segment. */
        immVertex2fv(attr_id.pos, t0);
        immVertex2fv(attr_id.pos, t1);
      }

      /* if last segment, also draw end of segment (defined as segment's normal) */
      if (i == totpoints - 2) {
        /* for once, we use second point's pressure (otherwise it won't be drawn) */
        pthick = (pt2->pressure * thickness * scalefac);

        /* color of point */
        gp_set_point_varying_color(pt2, ink, attr_id.color, false);

        /* calculate points for end of segment */
        mt[0] = m2[0] * pthick;
        mt[1] = m2[1] * pthick;

        t0[0] = s1[0] - mt[0];
        t0[1] = s1[1] - mt[1];
        t1[0] = s1[0] + mt[0];
        t1[1] = s1[1] + mt[1];

        /* Last two points of last segment (and first two points of end cap). */
        immVertex2fv(attr_id.pos, t0);
        immVertex2fv(attr_id.pos, t1);

        /* draw end cap as last step
         * - make points slightly closer to center (about halfway across)
         */
        mt[0] = m2[0] * pthick * 0.5f;
        mt[1] = m2[1] * pthick * 0.5f;
        sc[0] = s1[0] + (m1[0] * pthick * 0.75f);
        sc[1] = s1[1] + (m1[1] * pthick * 0.75f);

        t0[0] = sc[0] - mt[0];
        t0[1] = sc[1] - mt[1];
        t1[0] = sc[0] + mt[0];
        t1[1] = sc[1] + mt[1];

        /* Last two points of end cap. */
        immVertex2fv(attr_id.pos, t0);
        immVertex2fv(attr_id.pos, t1);
      }

      /* store computed point2 coordinates as point1 ones of next segment. */
      copy_v2_v2(s0, s1);
      /* store stroke's 'natural' normal for next stroke to use */
      copy_v2_v2(pm, m2);
    }

    immEnd();
    immUnbindProgram();
  }
}

/* ----- Strokes Drawing ------ */

/* Helper for doing all the checks on whether a stroke can be drawn */
static bool gp_can_draw_stroke(const bGPDstroke *gps, const int dflag)
{
  /* skip stroke if it isn't in the right display space for this drawing context */
  /* 1) 3D Strokes */
  if ((dflag & GP_DRAWDATA_ONLY3D) && !(gps->flag & GP_STROKE_3DSPACE)) {
    return false;
  }
  if (!(dflag & GP_DRAWDATA_ONLY3D) && (gps->flag & GP_STROKE_3DSPACE)) {
    return false;
  }

  /* 2) Screen Space 2D Strokes */
  if ((dflag & GP_DRAWDATA_ONLYV2D) && !(gps->flag & GP_STROKE_2DSPACE)) {
    return false;
  }
  if (!(dflag & GP_DRAWDATA_ONLYV2D) && (gps->flag & GP_STROKE_2DSPACE)) {
    return false;
  }

  /* 3) Image Space (2D) */
  if ((dflag & GP_DRAWDATA_ONLYI2D) && !(gps->flag & GP_STROKE_2DIMAGE)) {
    return false;
  }
  if (!(dflag & GP_DRAWDATA_ONLYI2D) && (gps->flag & GP_STROKE_2DIMAGE)) {
    return false;
  }

  /* skip stroke if it doesn't have any valid data */
  if ((gps->points == NULL) || (gps->totpoints < 1)) {
    return false;
  }

  /* stroke can be drawn */
  return true;
}

/* draw a set of strokes */
static void gp_draw_strokes(tGPDdraw *tgpw)
{
  float tcolor[4];
  float tfill[4];
  short sthickness;
  float ink[4];
  const bool is_unique = (tgpw->gps != NULL);
  const bool use_mat = (tgpw->gpd->mat != NULL);

  GPU_program_point_size(true);

  bGPDstroke *gps_init = (tgpw->gps) ? tgpw->gps : tgpw->t_gpf->strokes.first;

  for (bGPDstroke *gps = gps_init; gps; gps = gps->next) {
    /* check if stroke can be drawn */
    if (gp_can_draw_stroke(gps, tgpw->dflag) == false) {
      continue;
    }
    /* check if the color is visible */
    Material *ma = (use_mat) ? tgpw->gpd->mat[gps->mat_nr] : BKE_material_default_gpencil();
    MaterialGPencilStyle *gp_style = (ma) ? ma->gp_style : NULL;

    if ((gp_style == NULL) || (gp_style->flag & GP_MATERIAL_HIDE) ||
        /* if onion and ghost flag do not draw*/
        (tgpw->onion && (gp_style->flag & GP_MATERIAL_ONIONSKIN))) {
      continue;
    }

    /* if disable fill, the colors with fill must be omitted too except fill boundary strokes */
    if ((tgpw->disable_fill == 1) && (gp_style->fill_rgba[3] > 0.0f) &&
        ((gps->flag & GP_STROKE_NOFILL) == 0) && (gp_style->flag & GP_MATERIAL_FILL_SHOW)) {
      continue;
    }

    /* calculate thickness */
    sthickness = gps->thickness + tgpw->lthick;

    if (tgpw->is_fill_stroke) {
      sthickness = (short)max_ii(1, sthickness / 2);
    }

    if (sthickness <= 0) {
      continue;
    }

    /* check which stroke-drawer to use */
    if (tgpw->dflag & GP_DRAWDATA_ONLY3D) {
      const int no_xray = (tgpw->dflag & GP_DRAWDATA_NO_XRAY);
      int mask_orig = 0;

      if (no_xray) {
        glGetIntegerv(GL_DEPTH_WRITEMASK, &mask_orig);
        glDepthMask(0);
        GPU_depth_test(true);

        /* first arg is normally rv3d->dist, but this isn't
         * available here and seems to work quite well without */
        bglPolygonOffset(1.0f, 1.0f);
      }

      /* 3D Fill */
      // if ((dflag & GP_DRAWDATA_FILL) && (gps->totpoints >= 3)) {
      if ((gps->totpoints >= 3) && (tgpw->disable_fill != 1)) {
        /* set color using material, tint color and opacity */
        interp_v3_v3v3(tfill, gp_style->fill_rgba, tgpw->tintcolor, tgpw->tintcolor[3]);
        tfill[3] = gp_style->fill_rgba[3] * tgpw->opacity;
        if ((tfill[3] > GPENCIL_ALPHA_OPACITY_THRESH) || (gp_style->fill_style > 0)) {
          const float *color;
          if (!tgpw->onion) {
            color = tfill;
          }
          else {
            if (tgpw->custonion) {
              color = tgpw->tintcolor;
            }
            else {
              ARRAY_SET_ITEMS(tfill, UNPACK3(gp_style->fill_rgba), tgpw->tintcolor[3]);
              color = tfill;
            }
          }
          gp_draw_stroke_fill(tgpw->gpd,
                              gps,
                              tgpw->offsx,
                              tgpw->offsy,
                              tgpw->winx,
                              tgpw->winy,
                              tgpw->diff_mat,
                              color);
        }
      }

      /* 3D Stroke */
      /* set color using material tint color and opacity */
      if (!tgpw->onion) {
        interp_v3_v3v3(tcolor, gp_style->stroke_rgba, tgpw->tintcolor, tgpw->tintcolor[3]);
        tcolor[3] = gp_style->stroke_rgba[3] * tgpw->opacity;
        copy_v4_v4(ink, tcolor);
      }
      else {
        if (tgpw->custonion) {
          copy_v4_v4(ink, tgpw->tintcolor);
        }
        else {
          ARRAY_SET_ITEMS(tcolor, UNPACK3(gp_style->stroke_rgba), tgpw->opacity);
          copy_v4_v4(ink, tcolor);
        }
      }

      /* if used for fill, set opacity to 1 */
      if (tgpw->is_fill_stroke) {
        if (ink[3] >= GPENCIL_ALPHA_OPACITY_THRESH) {
          ink[3] = 1.0f;
        }
      }

      if (gp_style->mode == GP_MATERIAL_MODE_DOT) {
        /* volumetric stroke drawing */
        if (tgpw->disable_fill != 1) {
          gp_draw_stroke_volumetric_3d(gps->points, gps->totpoints, sthickness, ink);
        }
      }
      else {
        /* 3D Lines - OpenGL primitives-based */
        if (gps->totpoints == 1) {
          if (tgpw->disable_fill != 1) {
            gp_draw_stroke_point(gps->points,
                                 sthickness,
                                 tgpw->dflag,
                                 gps->flag,
                                 tgpw->offsx,
                                 tgpw->offsy,
                                 tgpw->winx,
                                 tgpw->winy,
                                 tgpw->diff_mat,
                                 ink);
          }
        }
        else {
          tgpw->gps = gps;
          gp_draw_stroke_3d(tgpw, sthickness, ink, gps->flag & GP_STROKE_CYCLIC);
        }
      }
      if (no_xray) {
        glDepthMask(mask_orig);
        GPU_depth_test(false);

        bglPolygonOffset(0.0, 0.0);
      }
    }
    else {
      /* 2D - Fill */
      if (gps->totpoints >= 3) {
        /* set color using material, tint color and opacity */
        interp_v3_v3v3(tfill, gp_style->fill_rgba, tgpw->tintcolor, tgpw->tintcolor[3]);
        tfill[3] = gp_style->fill_rgba[3] * tgpw->opacity;
        if ((tfill[3] > GPENCIL_ALPHA_OPACITY_THRESH) || (gp_style->fill_style > 0)) {
          const float *color;
          if (!tgpw->onion) {
            color = tfill;
          }
          else {
            if (tgpw->custonion) {
              color = tgpw->tintcolor;
            }
            else {
              ARRAY_SET_ITEMS(tfill, UNPACK3(gp_style->fill_rgba), tgpw->tintcolor[3]);
              color = tfill;
            }
          }
          gp_draw_stroke_fill(tgpw->gpd,
                              gps,
                              tgpw->offsx,
                              tgpw->offsy,
                              tgpw->winx,
                              tgpw->winy,
                              tgpw->diff_mat,
                              color);
        }
      }

      /* 2D Strokes... */
      /* set color using material, tint color and opacity */
      if (!tgpw->onion) {
        interp_v3_v3v3(tcolor, gp_style->stroke_rgba, tgpw->tintcolor, tgpw->tintcolor[3]);
        tcolor[3] = gp_style->stroke_rgba[3] * tgpw->opacity;
        copy_v4_v4(ink, tcolor);
      }
      else {
        if (tgpw->custonion) {
          copy_v4_v4(ink, tgpw->tintcolor);
        }
        else {
          ARRAY_SET_ITEMS(tcolor, UNPACK3(gp_style->stroke_rgba), tgpw->opacity);
          copy_v4_v4(ink, tcolor);
        }
      }
      if (gp_style->mode == GP_MATERIAL_MODE_DOT) {
        /* blob/disk-based "volumetric" drawing */
        gp_draw_stroke_volumetric_2d(gps->points,
                                     gps->totpoints,
                                     sthickness,
                                     tgpw->dflag,
                                     gps->flag,
                                     tgpw->offsx,
                                     tgpw->offsy,
                                     tgpw->winx,
                                     tgpw->winy,
                                     tgpw->diff_mat,
                                     ink);
      }
      else {
        /* normal 2D strokes */
        if (gps->totpoints == 1) {
          gp_draw_stroke_point(gps->points,
                               sthickness,
                               tgpw->dflag,
                               gps->flag,
                               tgpw->offsx,
                               tgpw->offsy,
                               tgpw->winx,
                               tgpw->winy,
                               tgpw->diff_mat,
                               ink);
        }
        else {
          gp_draw_stroke_2d(gps->points,
                            gps->totpoints,
                            sthickness,
                            tgpw->dflag,
                            gps->flag,
                            false,
                            tgpw->offsx,
                            tgpw->offsy,
                            tgpw->winx,
                            tgpw->winy,
                            tgpw->diff_mat,
                            ink);
        }
      }
    }
    /* if only one stroke, exit from loop */
    if (is_unique) {
      break;
    }
  }

  GPU_program_point_size(false);
}

/* ----- General Drawing ------ */

/* wrapper to draw strokes for filling operator */
void ED_gp_draw_fill(tGPDdraw *tgpw)
{
  gp_draw_strokes(tgpw);
}
