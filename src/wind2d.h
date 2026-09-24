#ifndef WIND2D_H
#define WIND2D_H

// Direct2D render path (RenderBackend=d2d), B2: cursor/overlay shapes first.

extern bool d2d_available(void);

// Bind a DC render target to hdc for one paint slice; false → caller uses GDI.
extern bool d2d_begin(HDC hdc);
extern void d2d_end(void);

// Axis-aligned rects in device pixels; alpha 0..255 (true alpha, not blend_colour).
extern void d2d_fill_rect(float x, float y, float w, float h, colour c, int alpha);
extern void d2d_stroke_rect(float x, float y, float w, float h, colour c,
                            int alpha, float stroke);

// Drop cached factory objects (font/window teardown).
extern void d2d_shutdown(void);

#endif
