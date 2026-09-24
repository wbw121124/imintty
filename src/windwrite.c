// windwrite.c (part of imintty)
// DirectWrite shaping for FontRender=dwrite (B1): DWrite shapes, GDI draws.
// Licensed under the terms of the GNU General Public License v3 or later.

#define COBJMACROS
#include <initguid.h>
#include <dwrite.h>

#include "winpriv.h"
#include "config.h"
#include "charset.h"  // cs__wcstoutf
#include "windwrite.h"
#include "wind2d.h"

// IID without linking dwrite.lib GUID (same workaround as prior cygwin code).
static const IID MY_IID_IDWriteFactory =
  { 0xb859ee5a, 0xd838, 0x4b5b,
    { 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48 } };

static IDWriteFactory * dw_factory;
static IDWriteGdiInterop * dw_interop;
static IDWriteTextAnalyzer * dw_analyzer;

// Shaped run for the current text_out pair (one shape per full-run draw).
static bool dw_run_active;
static IDWriteFontFace * dw_run_face;
static UINT16 * dw_run_glyphs;
static INT * dw_run_adv;       // cell-snapped advances (px), length dw_run_count
static UINT32 dw_run_count;
static wchar * dw_run_text;    // copy of shaped text for identity checks
static int dw_run_len;

static bool
dw_ensure(void)
{
  if (dw_factory)
    return true;
  HRESULT hr = DWriteCreateFactory(
                 DWRITE_FACTORY_TYPE_SHARED,
                 &MY_IID_IDWriteFactory,
                 (IUnknown **)&dw_factory);
  if (FAILED(hr)) {
    dw_factory = 0;
    return false;
  }
  hr = IDWriteFactory_GetGdiInterop(dw_factory, &dw_interop);
  if (FAILED(hr)) {
    IDWriteFactory_Release(dw_factory);
    dw_factory = 0;
    dw_interop = 0;
    return false;
  }
  hr = IDWriteFactory_CreateTextAnalyzer(dw_factory, &dw_analyzer);
  if (FAILED(hr)) {
    IDWriteGdiInterop_Release(dw_interop);
    IDWriteFactory_Release(dw_factory);
    dw_factory = 0;
    dw_interop = 0;
    dw_analyzer = 0;
    return false;
  }
  return true;
}

void
dw_font_changed(void)
{
  if (dw_run_face) {
    IDWriteFontFace_Release(dw_run_face);
    dw_run_face = 0;
  }
}

static IDWriteFontFace *
dw_face_from_hdc(HDC hdc)
{
  if (!dw_ensure() || !dw_interop)
    return 0;
  IDWriteFontFace * face = 0;
  HRESULT hr = IDWriteGdiInterop_CreateFontFaceFromHdc(dw_interop, hdc, &face);
  if (FAILED(hr))
    return 0;
  return face;
}

// ---- IDWriteTextAnalysisSource over a fixed UTF-16 buffer ----

typedef struct {
  IDWriteTextAnalysisSource base;
  ULONG ref;
  const WCHAR * text;
  UINT32 length;
} dw_analysis_source;

static HRESULT STDMETHODCALLTYPE
src_QueryInterface(IDWriteTextAnalysisSource *This, REFIID riid, void **ppv)
{
  if (!ppv)
    return E_POINTER;
  if (IsEqualGUID(riid, &IID_IUnknown) ||
      IsEqualGUID(riid, &IID_IDWriteTextAnalysisSource)) {
    *ppv = This;
    This->lpVtbl->AddRef(This);
    return S_OK;
  }
  *ppv = 0;
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE
src_AddRef(IDWriteTextAnalysisSource *This)
{
  dw_analysis_source * s = (dw_analysis_source *)This;
  return ++s->ref;
}

static ULONG STDMETHODCALLTYPE
src_Release(IDWriteTextAnalysisSource *This)
{
  dw_analysis_source * s = (dw_analysis_source *)This;
  if (--s->ref)
    return s->ref;
  free(s);
  return 0;
}

static HRESULT STDMETHODCALLTYPE
src_GetTextAtPosition(IDWriteTextAnalysisSource *This, UINT32 position,
                      const WCHAR **text, UINT32 *text_len)
{
  dw_analysis_source * s = (dw_analysis_source *)This;
  if (position >= s->length) {
    *text = 0;
    *text_len = 0;
    return S_OK;
  }
  *text = s->text + position;
  *text_len = s->length - position;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE
src_GetTextBeforePosition(IDWriteTextAnalysisSource *This, UINT32 position,
                          const WCHAR **text, UINT32 *text_len)
{
  dw_analysis_source * s = (dw_analysis_source *)This;
  if (position == 0 || position > s->length) {
    *text = 0;
    *text_len = 0;
    return S_OK;
  }
  *text = s->text;
  *text_len = position;
  return S_OK;
}

static DWRITE_READING_DIRECTION STDMETHODCALLTYPE
src_GetParagraphReadingDirection(IDWriteTextAnalysisSource *This)
{
  (void)This;
  return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
}

static HRESULT STDMETHODCALLTYPE
src_GetLocaleName(IDWriteTextAnalysisSource *This, UINT32 position,
                  UINT32 *text_len, const WCHAR **locale)
{
  static const WCHAR en[] = L"en-us";
  (void)This;
  (void)position;
  *locale = en;
  *text_len = 5;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE
src_GetNumberSubstitution(IDWriteTextAnalysisSource *This, UINT32 position,
                          UINT32 *text_len,
                          IDWriteNumberSubstitution **substitution)
{
  (void)This;
  (void)position;
  (void)text_len;
  *substitution = 0;
  return S_OK;
}

static const IDWriteTextAnalysisSourceVtbl dw_analysis_source_vtbl = {
  src_QueryInterface,
  src_AddRef,
  src_Release,
  src_GetTextAtPosition,
  src_GetTextBeforePosition,
  src_GetParagraphReadingDirection,
  src_GetLocaleName,
  src_GetNumberSubstitution
};

static dw_analysis_source *
dw_make_source(const wchar * text, int len)
{
  dw_analysis_source * s = calloc(1, sizeof *s);
  if (!s)
    return 0;
  s->base.lpVtbl = (IDWriteTextAnalysisSourceVtbl *)&dw_analysis_source_vtbl;
  s->ref = 1;
  s->text = text;
  s->length = (UINT32)len;
  return s;
}

// ---- IDWriteTextAnalysisSink ----

typedef struct {
  IDWriteTextAnalysisSink base;
  ULONG ref;
  DWRITE_SCRIPT_ANALYSIS sa;
  bool has_sa;
} dw_analysis_sink;

static HRESULT STDMETHODCALLTYPE
snk_QueryInterface(IDWriteTextAnalysisSink *This, REFIID riid, void **ppv)
{
  if (!ppv)
    return E_POINTER;
  if (IsEqualGUID(riid, &IID_IUnknown) ||
      IsEqualGUID(riid, &IID_IDWriteTextAnalysisSink)) {
    *ppv = This;
    This->lpVtbl->AddRef(This);
    return S_OK;
  }
  *ppv = 0;
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE
snk_AddRef(IDWriteTextAnalysisSink *This)
{
  dw_analysis_sink * s = (dw_analysis_sink *)This;
  return ++s->ref;
}

static ULONG STDMETHODCALLTYPE
snk_Release(IDWriteTextAnalysisSink *This)
{
  dw_analysis_sink * s = (dw_analysis_sink *)This;
  if (--s->ref)
    return s->ref;
  free(s);
  return 0;
}

static HRESULT STDMETHODCALLTYPE
snk_SetScriptAnalysis(IDWriteTextAnalysisSink *This, UINT32 position,
                      UINT32 length,
                      const DWRITE_SCRIPT_ANALYSIS *scriptanalysis)
{
  dw_analysis_sink * s = (dw_analysis_sink *)This;
  if (!s->has_sa && position == 0) {
    s->sa = *scriptanalysis;
    s->has_sa = true;
  }
  (void)length;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE
snk_SetLineBreakpoints(IDWriteTextAnalysisSink *This, UINT32 position,
                       UINT32 length,
                       const DWRITE_LINE_BREAKPOINT *breakpoints)
{
  (void)This; (void)position; (void)length; (void)breakpoints;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE
snk_SetBidiLevel(IDWriteTextAnalysisSink *This, UINT32 position,
                 UINT32 length, UINT8 explicitLevel, UINT8 resolvedLevel)
{
  (void)This; (void)position; (void)length;
  (void)explicitLevel; (void)resolvedLevel;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE
snk_SetNumberSubstitution(IDWriteTextAnalysisSink *This, UINT32 position,
                          UINT32 length,
                          IDWriteNumberSubstitution *substitution)
{
  (void)This; (void)position; (void)length; (void)substitution;
  return S_OK;
}

static const IDWriteTextAnalysisSinkVtbl dw_analysis_sink_vtbl = {
  snk_QueryInterface,
  snk_AddRef,
  snk_Release,
  snk_SetScriptAnalysis,
  snk_SetLineBreakpoints,
  snk_SetBidiLevel,
  snk_SetNumberSubstitution
};

static dw_analysis_sink *
dw_make_sink(void)
{
  dw_analysis_sink * s = calloc(1, sizeof *s);
  if (!s)
    return 0;
  s->base.lpVtbl = (IDWriteTextAnalysisSinkVtbl *)&dw_analysis_sink_vtbl;
  s->ref = 1;
  return s;
}

static void
dw_release_run(void)
{
  if (dw_run_face) {
    IDWriteFontFace_Release(dw_run_face);
    dw_run_face = 0;
  }
  free(dw_run_glyphs);
  dw_run_glyphs = 0;
  free(dw_run_adv);
  dw_run_adv = 0;
  free(dw_run_text);
  dw_run_text = 0;
  dw_run_count = 0;
  dw_run_len = 0;
  dw_run_active = false;
}

// Parse FontFeatures like "ss01,zero,calt" into OpenType tags (1..n).
static int
dw_parse_features(DWRITE_FONT_FEATURE * out, int maxn)
{
  if (!cfg.font_features || !*cfg.font_features)
    return 0;
  int n = 0;
  char * s = cs__wcstoutf(cfg.font_features);
  if (!s)
    return 0;
  for (char * p = s; *p && n < maxn; ) {
    while (*p == ' ' || *p == ',')
      p++;
    if (!*p)
      break;
    char * q = p;
    while (*q && *q != ',')
      q++;
    char save = *q;
    *q = 0;
    // trim trailing spaces
    char * e = q;
    while (e > p && e[-1] == ' ')
      e--;
    *e = 0;
    int len = (int)strlen(p);
    if (len == 4) {
      out[n].nameTag = (DWRITE_FONT_FEATURE_TAG)
        ((uchar)p[0] | ((uchar)p[1] << 8) | ((uchar)p[2] << 16) |
         ((uchar)p[3] << 24));
      out[n].parameter = 1;
      n++;
    }
    *q = save;
    p = q;
    if (*p == ',')
      p++;
  }
  free(s);
  return n;
}

// Map shaped glyphs to cell advances using input per-character dxs[].
// clusterMap[i] = cluster id for character i; glyphs belong to clusters in order.
static bool
dw_map_cluster_cells(const UINT16 * cluster_map, const UINT16 * glyphs,
                     UINT32 nglyphs, int len, const int * dxs,
                     INT * adv_out)
{
  if (nglyphs == 0)
    return false;

  // Group consecutive characters with the same cluster id; each group is one
  // cluster. For LTR monospace, glyph k covers the k-th cluster in order.
  // When mark glyphs split a cluster, merge extras into the same advance.
  int nclusters = 0;
  int * csum = newn(int, len + 1);
  if (!csum)
    return false;

  int cur = cluster_map[0];
  int sum = dxs[0];
  for (int i = 1; i < len; i++) {
    if (cluster_map[i] != cur) {
      csum[nclusters++] = sum;
      cur = cluster_map[i];
      sum = dxs[i];
    }
    else
      sum += dxs[i];
  }
  csum[nclusters++] = sum;

  if ((UINT32)nclusters == nglyphs) {
    for (UINT32 g = 0; g < nglyphs; g++)
      adv_out[g] = csum[g];
    free(csum);
    return true;
  }

  // Uneven cluster/glyph count: distribute cluster advances across glyphs
  // that share a cluster by walking cluster_map → glyph ordinal.
  // Fallback: equal split of total width (keeps monospace contract).
  if (nclusters == 0) {
    free(csum);
    return false;
  }

  // Build per-glyph advance: for each glyph, find first char whose cluster
  // start ordinal maps to it. Approximate: assign full cell dxs sequence
  // by character index remapped through cluster starts.
  // Safer: total width / nglyphs when mismatch (rare with liga only).
  int total = 0;
  for (int i = 0; i < len; i++)
    total += dxs[i];
  free(csum);
  (void)glyphs;
  if (nglyphs == 1) {
    adv_out[0] = total;
    return true;
  }
  // leave advances zero and signal failure → caller falls back to text ExtTextOut
  return false;
}

bool
dw_text_start(HDC hdc, wchar *text, int len, int *dxs)
{
  dw_release_run();
  if (cfg.font_render != FR_DWRITE || !text || len <= 0 || !dxs)
    return false;
  if (!dw_ensure())
    return false;

  IDWriteFontFace * face = dw_face_from_hdc(hdc);
  if (!face)
    return false;

  dw_analysis_source * src = dw_make_source(text, len);
  dw_analysis_sink * snk = dw_make_sink();
  if (!src || !snk) {
    if (src)
      src->base.lpVtbl->Release(&src->base);
    if (snk)
      snk->base.lpVtbl->Release(&snk->base);
    IDWriteFontFace_Release(face);
    return false;
  }

  HRESULT hr = IDWriteTextAnalyzer_AnalyzeScript(
                 dw_analyzer, &src->base, 0, (UINT32)len, &snk->base);
  if (FAILED(hr) || !snk->has_sa) {
    src->base.lpVtbl->Release(&src->base);
    snk->base.lpVtbl->Release(&snk->base);
    IDWriteFontFace_Release(face);
    return false;
  }

  UINT32 max_glyphs = (UINT32)len * 3 + 16;
  UINT16 * glyphs = calloc(max_glyphs, sizeof *glyphs);
  UINT16 * cluster_map = calloc((UINT32)len, sizeof *cluster_map);
  DWRITE_SHAPING_TEXT_PROPERTIES * text_props =
    calloc((UINT32)len, sizeof *text_props);
  DWRITE_SHAPING_GLYPH_PROPERTIES * glyph_props =
    calloc(max_glyphs, sizeof *glyph_props);
  if (!glyphs || !cluster_map || !text_props || !glyph_props) {
    free(glyphs);
    free(cluster_map);
    free(text_props);
    free(glyph_props);
    src->base.lpVtbl->Release(&src->base);
    snk->base.lpVtbl->Release(&snk->base);
    IDWriteFontFace_Release(face);
    return false;
  }

  DWRITE_FONT_FEATURE feats[8];
  int nfeat = dw_parse_features(feats, lengthof(feats));
  DWRITE_TYPOGRAPHIC_FEATURES featset = { feats, (UINT32)nfeat };
  const DWRITE_TYPOGRAPHIC_FEATURES * featptr = nfeat ? &featset : 0;
  UINT32 feat_len = nfeat ? (UINT32)len : 0;
  UINT32 feat_ranges = nfeat ? 1 : 0;

  // Also force liga/calt on when Ligatures>1 and no explicit FontFeatures
  // (matches Uniscribe sctrl_lig behaviour for #601).
  DWRITE_FONT_FEATURE liga[2];
  if (!nfeat && cfg.ligatures > 1) {
    liga[0].nameTag = DWRITE_FONT_FEATURE_TAG_STANDARD_LIGATURES;
    liga[0].parameter = 1;
    liga[1].nameTag = DWRITE_FONT_FEATURE_TAG_CONTEXTUAL_ALTERNATES;
    liga[1].parameter = 1;
    featset.features = liga;
    featset.featureCount = 2;
    featptr = &featset;
    feat_len = (UINT32)len;
    feat_ranges = 1;
  }

  UINT32 nglyphs = 0;
  hr = IDWriteTextAnalyzer_GetGlyphs(
         dw_analyzer,
         text, (UINT32)len,
         face,
         FALSE, FALSE,
         &snk->sa,
         0,  // locale: default from source not required for GetGlyphs
         0,  // number substitution
         featptr ? &featptr : 0,
         featptr ? &feat_len : 0,
         feat_ranges,
         max_glyphs,
         cluster_map,
         text_props,
         glyphs,
         glyph_props,
         &nglyphs);

  if (FAILED(hr) || nglyphs == 0) {
    free(glyphs);
    free(cluster_map);
    free(text_props);
    free(glyph_props);
    src->base.lpVtbl->Release(&src->base);
    snk->base.lpVtbl->Release(&snk->base);
    IDWriteFontFace_Release(face);
    return false;
  }

  INT * adv = calloc(nglyphs, sizeof *adv);
  bool mapped = adv &&
    dw_map_cluster_cells(cluster_map, glyphs, nglyphs, len, dxs, adv);

  // release analysis intermediates; keep face + glyphs + adv for draw
  free(cluster_map);
  free(text_props);
  free(glyph_props);
  src->base.lpVtbl->Release(&src->base);
  snk->base.lpVtbl->Release(&snk->base);

  if (!mapped) {
    free(adv);
    free(glyphs);
    IDWriteFontFace_Release(face);
    return false;
  }

  dw_run_face = face;
  dw_run_glyphs = glyphs;
  dw_run_adv = adv;
  dw_run_count = nglyphs;
  dw_run_text = newn(wchar, len);
  if (dw_run_text)
    wmemcpy(dw_run_text, text, len);
  dw_run_len = len;
  dw_run_active = true;
  return true;
}

bool
dw_text_out(HDC hdc, int x, int y, UINT eto, RECT *box,
            wchar *text, int len, int *dxs)
{
  if (!dw_run_active || !dw_run_glyphs || dw_run_count == 0)
    return false;

  // Only draw when this call matches the shaped run (full-run path).
  // Substring draws (combining stack) fall back to caller ExtTextOutW.
  if (len != dw_run_len || !dw_run_text ||
      wmemcmp(text, dw_run_text, len) != 0)
    return false;

  // Glyph array is always glyph ids here; keep ETO_GLYPH_INDEX for the GDI fallback.
  UINT flags = eto & (ETO_OPAQUE | ETO_CLIPPED);
  flags |= ETO_GLYPH_INDEX;

  // Prefer D2D DrawGlyphRun whenever the factory is available (B2/B4).
  {
    LOGFONTW lf;
    HFONT hf = (HFONT)GetCurrentObject(hdc, OBJ_FONT);
    float em = 0;
    TEXTMETRICW tm;
    bool have_tm = GetTextMetricsW(hdc, &tm) != 0;
    // GDI cell height is winA+winD scaled; design em = cell * upem / (winA+winD).
    if (dw_run_face) {
      DWRITE_FONT_METRICS fm;
      IDWriteFontFace_GetMetrics(dw_run_face, &fm);
      UINT16 winA = fm.ascent, winD = fm.descent;
      UINT16 winH = winA + winD;
      if (winH && have_tm && tm.tmHeight > 0 && fm.designUnitsPerEm)
        em = (float)tm.tmHeight * (float)fm.designUnitsPerEm / (float)winH;
    }
    if (em <= 0 && hf && GetObjectW(hf, sizeof lf, &lf))
      em = (float)(lf.lfHeight < 0 ? -lf.lfHeight : lf.lfHeight);
    if (em <= 0 && have_tm)
      em = (float)tm.tmHeight;
    // GDI TA_TOP: y is cell top; baseline is top + ascent.
    float baseline = (float)y;
    if (have_tm)
      baseline += (float)tm.tmAscent;
    if (em > 0 && d2d_draw_glyph_run(hdc, dw_run_face, dw_run_glyphs,
                                     dw_run_adv, (unsigned)dw_run_count,
                                     (float)x, baseline, em,
                                     GetTextColor(hdc), flags, box))
      return true;
    // fall through to GDI if D2D bind/draw failed
  }

  BOOL ok = ExtTextOutW(hdc, x, y, flags, box,
                        (LPCWSTR)dw_run_glyphs, (UINT)dw_run_count,
                        dw_run_adv);
  (void)dxs;
  return ok != FALSE;
}

void
dw_text_end(void)
{
  dw_release_run();
}

int
dw_char_advance(HDC hdc, wchar wc)
{
  IDWriteFontFace * face = dw_face_from_hdc(hdc);
  if (!face)
    return 0;
  UINT32 cp = wc;
  UINT16 g = 0;
  HRESULT hr = IDWriteFontFace_GetGlyphIndices(face, &cp, 1, &g);
  int adv = 0;
  if (SUCCEEDED(hr) && g) {
    DWRITE_GLYPH_METRICS gm;
    hr = IDWriteFontFace_GetDesignGlyphMetrics(face, &g, 1, &gm, FALSE);
    if (SUCCEEDED(hr) && gm.advanceWidth && cell_height > 0) {
      DWRITE_FONT_METRICS fm;
      IDWriteFontFace_GetMetrics(face, &fm);
      float upem = (float)fm.designUnitsPerEm;
      if (upem > 0)
        adv = (int)(gm.advanceWidth * (float)cell_height / upem + 0.5f);
    }
  }
  IDWriteFontFace_Release(face);
  return adv;
}
