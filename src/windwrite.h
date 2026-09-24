#ifndef WINDWRITE_H
#define WINDWRITE_H

// DirectWrite text path (FontRender=dwrite), B1: shape via DWrite, draw via GDI.

// Shape text[0..len) with the font currently selected on hdc.
// Returns false if DWrite unavailable or shaping failed (caller falls back).
extern bool dw_text_start(HDC hdc, wchar *text, int len, int *dxs);
extern bool dw_text_out(HDC hdc, int x, int y, UINT eto, RECT *box,
                        wchar *text, int len, int *dxs);
extern void dw_text_end(void);

// Drop cached factory objects when HFONTs are recreated.
extern void dw_font_changed(void);

// Optional advance width in pixels from glyph metrics (0 if unavailable).
extern int dw_char_advance(HDC hdc, wchar wc);

#endif
