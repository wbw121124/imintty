// wind2d.c (part of imintty)
// Direct2D path for RenderBackend=d2d (B2): cursor/overlay shapes first.
// Uses ID2D1DCRenderTarget bound to the paint HDC so GDI text still shows.
// Licensed under the terms of the GNU General Public License v3 or later.

#define COBJMACROS
#include <initguid.h>
#include <d2d1.h>
#include <dwrite.h>

#include "winpriv.h"
#include "config.h"
#include "wind2d.h"

static ID2D1Factory * d2d_factory;
static ID2D1DCRenderTarget * d2d_rt;
static bool d2d_drawing;

static D2D1_COLOR_F
d2d_colour(colour c, int alpha)
{
  if (alpha < 0)
    alpha = 0;
  if (alpha > 255)
    alpha = 255;
  D2D1_COLOR_F col = {
    .r = red(c) / 255.0f,
    .g = green(c) / 255.0f,
    .b = blue(c) / 255.0f,
    .a = alpha / 255.0f
  };
  return col;
}

static bool
d2d_ensure(void)
{
  if (d2d_factory && d2d_rt)
    return true;
  if (!d2d_factory) {
    HRESULT hr = D2D1CreateFactory(
                   D2D1_FACTORY_TYPE_SINGLE_THREADED,
                   &IID_ID2D1Factory,
                   0,
                   (void **)&d2d_factory);
    if (FAILED(hr)) {
      d2d_factory = 0;
      return false;
    }
  }
  if (!d2d_rt) {
    D2D1_RENDER_TARGET_PROPERTIES rtp = {
      .type = D2D1_RENDER_TARGET_TYPE_DEFAULT,
      .pixelFormat = {.format = DXGI_FORMAT_UNKNOWN,
                      .alphaMode = D2D1_ALPHA_MODE_UNKNOWN},
      .dpiX = 0,
      .dpiY = 0,
      .usage = D2D1_RENDER_TARGET_USAGE_NONE,
      .minLevel = D2D1_FEATURE_LEVEL_DEFAULT
    };
    HRESULT hr = ID2D1Factory_CreateDCRenderTarget(d2d_factory, &rtp, &d2d_rt);
    if (FAILED(hr)) {
      d2d_rt = 0;
      return false;
    }
  }
  return true;
}

bool
d2d_available(void)
{
  /* Always prefer D2D when the OS can create the factory; GDI is only
     for systems where Direct2D init fails (or explicit RenderBackend=gdi). */
  if (cfg.render_backend == RB_GDI)
    return false;
  return d2d_ensure();
}

bool
d2d_begin(HDC hdc)
{
  if (!hdc || d2d_drawing)
    return false;
  if (cfg.render_backend == RB_GDI)
    return false;
  if (!d2d_ensure())
    return false;
  HRESULT hr = ID2D1DCRenderTarget_BindDC(d2d_rt, hdc, 0);
  if (FAILED(hr))
    return false;
  ID2D1DCRenderTarget_BeginDraw(d2d_rt);
  d2d_drawing = true;
  return true;
}

void
d2d_end(void)
{
  if (!d2d_drawing)
    return;
  d2d_drawing = false;
  D2D1_TAG tag1 = 0, tag2 = 0;
  HRESULT hr = ID2D1DCRenderTarget_EndDraw(d2d_rt, &tag1, &tag2);
  if (hr == D2DERR_RECREATE_TARGET) {
    ID2D1DCRenderTarget_Release(d2d_rt);
    d2d_rt = 0;
  }
}

static ID2D1SolidColorBrush *
d2d_brush(colour c, int alpha)
{
  if (!d2d_drawing)
    return 0;
  D2D1_COLOR_F col = d2d_colour(c, alpha);
  D2D1_BRUSH_PROPERTIES bp = {.opacity = 1.0f};
  // identity matrix via nested union braces
  bp.transform.m[0][0] = 1.0f; bp.transform.m[0][1] = 0.0f;
  bp.transform.m[1][0] = 0.0f; bp.transform.m[1][1] = 1.0f;
  bp.transform.m[2][0] = 0.0f; bp.transform.m[2][1] = 0.0f;
  ID2D1SolidColorBrush * brush = 0;
  HRESULT hr = ID2D1DCRenderTarget_CreateSolidColorBrush(
                 d2d_rt, &col, &bp, &brush);
  if (FAILED(hr))
    return 0;
  return brush;
}

void
d2d_fill_rect(float x, float y, float w, float h, colour c, int alpha)
{
  if (!d2d_drawing || w <= 0 || h <= 0)
    return;
  ID2D1SolidColorBrush * brush = d2d_brush(c, alpha);
  if (!brush)
    return;
  D2D1_RECT_F rect = {.left = x, .top = y, .right = x + w, .bottom = y + h};
  ID2D1DCRenderTarget_FillRectangle(d2d_rt, &rect, (ID2D1Brush *)brush);
  ID2D1SolidColorBrush_Release(brush);
}

void
d2d_stroke_rect(float x, float y, float w, float h, colour c,
                int alpha, float stroke)
{
  if (!d2d_drawing || w <= 0 || h <= 0)
    return;
  if (stroke < 1.0f)
    stroke = 1.0f;
  ID2D1SolidColorBrush * brush = d2d_brush(c, alpha);
  if (!brush)
    return;
  D2D1_RECT_F rect = {.left = x, .top = y, .right = x + w, .bottom = y + h};
  ID2D1DCRenderTarget_DrawRectangle(d2d_rt, &rect, (ID2D1Brush *)brush,
                                    stroke, 0);
  ID2D1SolidColorBrush_Release(brush);
}

static bool
d2d_open_path(const float *xy, int n, bool filled,
              ID2D1PathGeometry **out_path, ID2D1GeometrySink **out_sink)
{
  *out_path = 0;
  *out_sink = 0;
  if (!d2d_drawing || n < 3)
    return false;
  ID2D1PathGeometry * path = 0;
  HRESULT hr = ID2D1Factory_CreatePathGeometry(d2d_factory, &path);
  if (FAILED(hr) || !path)
    return false;
  ID2D1GeometrySink * sink = 0;
  hr = ID2D1PathGeometry_Open(path, &sink);
  if (FAILED(hr) || !sink) {
    ID2D1PathGeometry_Release(path);
    return false;
  }
  D2D1_POINT_2F p0 = {.x = xy[0], .y = xy[1]};
  ID2D1GeometrySink_BeginFigure(
    sink, p0,
    filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
  for (int i = 1; i < n; i++) {
    D2D1_POINT_2F p = {.x = xy[i * 2], .y = xy[i * 2 + 1]};
    ID2D1GeometrySink_AddLine(sink, p);
  }
  ID2D1GeometrySink_EndFigure(sink, D2D1_FIGURE_END_CLOSED);
  *out_path = path;
  *out_sink = sink;
  return true;
}

void
d2d_fill_polygon(const float *xy, int n, colour c, int alpha)
{
  ID2D1PathGeometry * path = 0;
  ID2D1GeometrySink * sink = 0;
  if (!d2d_open_path(xy, n, true, &path, &sink))
    return;
  ID2D1GeometrySink_Close(sink);
  ID2D1GeometrySink_Release(sink);
  ID2D1SolidColorBrush * brush = d2d_brush(c, alpha);
  if (brush) {
    ID2D1DCRenderTarget_FillGeometry(d2d_rt, (ID2D1Geometry *)path,
                                     (ID2D1Brush *)brush, 0);
    ID2D1SolidColorBrush_Release(brush);
  }
  ID2D1PathGeometry_Release(path);
}

void
d2d_stroke_polygon(const float *xy, int n, colour c, int alpha, float stroke)
{
  if (stroke < 1.0f)
    stroke = 1.0f;
  ID2D1PathGeometry * path = 0;
  ID2D1GeometrySink * sink = 0;
  if (!d2d_open_path(xy, n, false, &path, &sink))
    return;
  ID2D1GeometrySink_Close(sink);
  ID2D1GeometrySink_Release(sink);
  ID2D1SolidColorBrush * brush = d2d_brush(c, alpha);
  if (brush) {
    ID2D1DCRenderTarget_DrawGeometry(d2d_rt, (ID2D1Geometry *)path,
                                     (ID2D1Brush *)brush, stroke, 0);
    ID2D1SolidColorBrush_Release(brush);
  }
  ID2D1PathGeometry_Release(path);
}

bool
d2d_draw_glyph_run(HDC hdc, void *font_face,
                   const unsigned short *glyphs,
                   const int *advances, unsigned count,
                   float x, float baseline, float em_size,
                   colour fg, unsigned eto, const RECT *clip)
{
  if (!font_face || !glyphs || !advances || count == 0 || em_size <= 0)
    return false;
  if (!d2d_begin(hdc))
    return false;

  if ((eto & ETO_OPAQUE) && clip) {
    colour bg = GetBkColor(hdc);
    d2d_fill_rect((float)clip->left, (float)clip->top,
                  (float)(clip->right - clip->left),
                  (float)(clip->bottom - clip->top), bg, 255);
  }

  if ((eto & ETO_CLIPPED) && clip) {
    D2D1_RECT_F cr = {
      .left = (float)clip->left, .top = (float)clip->top,
      .right = (float)clip->right, .bottom = (float)clip->bottom
    };
    ID2D1DCRenderTarget_PushAxisAlignedClip(
      d2d_rt, &cr, D2D1_ANTIALIAS_MODE_ALIASED);
  }

  ID2D1SolidColorBrush * brush = d2d_brush(fg, 255);
  bool ok = false;
  if (brush) {
    FLOAT * adv = 0;
    if (count) {
      adv = malloc(count * sizeof *adv);
      if (adv)
        for (unsigned i = 0; i < count; i++)
          adv[i] = (FLOAT)advances[i];
    }
    if (adv) {
      DWRITE_GLYPH_RUN run = {
        .fontFace = (IDWriteFontFace *)font_face,
        .fontEmSize = em_size,
        .glyphCount = count,
        .glyphIndices = glyphs,
        .glyphAdvances = adv,
        .glyphOffsets = 0,
        .isSideways = FALSE,
        .bidiLevel = 0
      };
      D2D1_POINT_2F origin = {.x = x, .y = baseline};
      ID2D1DCRenderTarget_DrawGlyphRun(
        d2d_rt, origin, &run, (ID2D1Brush *)brush,
        DWRITE_MEASURING_MODE_NATURAL);
      ok = true;
      free(adv);
    }
    ID2D1SolidColorBrush_Release(brush);
  }

  if ((eto & ETO_CLIPPED) && clip)
    ID2D1DCRenderTarget_PopAxisAlignedClip(d2d_rt);
  d2d_end();
  return ok;
}

void
d2d_shutdown(void)
{
  d2d_drawing = false;
  if (d2d_rt) {
    ID2D1DCRenderTarget_Release(d2d_rt);
    d2d_rt = 0;
  }
  if (d2d_factory) {
    ID2D1Factory_Release(d2d_factory);
    d2d_factory = 0;
  }
}
