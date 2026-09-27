#ifndef WIND2D_H
#define WIND2D_H

// Direct2D render path (RenderBackend=d2d), B2: cursor/overlay shapes first.
// P4: HwndRenderTarget binds to main window once; single-frame composition
// replaces per-call DCRenderTarget bind/unbind.

extern bool d2d_available(void);

// Bind a DC render target to hdc for one paint slice; false → caller uses GDI.
extern bool d2d_begin(HDC hdc);
extern void d2d_end(void);

// P4: Begin a single-frame D2D composition on HwndRenderTarget.
// All subsequent d2d_* calls go to the same frame until d2d_end().
// Returns false if HwndRT unavailable (falls back to GDI for that frame).
extern bool d2d_begin_hwnd(void);

// P4: Bind/rebind HwndRenderTarget to window. Call on creation, resize, DPI.
extern void d2d_set_hwnd(HWND hwnd);

// Axis-aligned rects in device pixels; alpha 0..255 (true alpha, not blend_colour).
extern void d2d_fill_rect(float x, float y, float w, float h, colour c, int alpha);
extern void d2d_stroke_rect(float x, float y, float w, float h, colour c,
                            int alpha, float stroke);

// Convex quad/polygon (x0,y0 .. x{n-1},y{n-1}); n >= 3. Smear cursor body.
extern void d2d_fill_polygon(const float *xy, int n, colour c, int alpha);
extern void d2d_stroke_polygon(const float *xy, int n, colour c, int alpha,
                               float stroke);

// Draw a pre-shaped glyph run (B1 DirectWrite output) via ID2D1 DrawGlyphRun.
// Advances are absolute pixels; baseline is the text baseline in device px.
// When d2d_begin_hwnd() is active, HDC is ignored and draws to HwndRT.
extern bool d2d_draw_glyph_run(HDC hdc, void *font_face,
                               const unsigned short *glyphs,
                               const int *advances, unsigned count,
                               float x, float baseline, float em_size,
                               colour fg, unsigned eto, const RECT *clip);

// Drop cached factory objects (font/window teardown).
extern void d2d_shutdown(void);

#endif
