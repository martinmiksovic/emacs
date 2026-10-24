/* Deterministic remote display recording, version 1.  GPL-3.0-or-later.
   These callbacks copy only scalar drawing data.  Emacs owns all layout.
   No Lisp allocation, pointer serialization, font/face retention or RPC. */
#include <config.h>
#include <stdarg.h>
#include "lisp.h"
#include "remoteterm.h"
#include "remoterecord.h"
#include "remotestream.h"
#include "window.h"
#include "buffer.h"
#include "blockinput.h"

#define U(x) ((long long) (x) * 1024)
#define HASH "a54dca07c76d6289e717e75e0a58c0128f6d7269ef3faf76417c9d7d3bba37ab"

static void part (struct frame *, const char *, ...) ATTRIBUTE_FORMAT_PRINTF (2, 3);
static void
part (struct frame *f, const char *format, ...)
{
  struct remote_display_info *d = FRAME_DISPLAY_INFO (f);
  if (!d->record_ready) return;
  va_list ap;
  va_start (ap, format);
  if (d->record && !d->record_failed && !d->snapshot_only)
    {
      va_list file_ap;
      va_copy (file_ap, ap);
      if (vfprintf (d->record, format, file_ap) < 0) d->record_failed = true;
      va_end (file_ap);
    }
  remote_stream_part (d, format, ap);
  va_end (ap);
}
static void start (struct frame *f, const char *event)
{
  struct remote_display_info *d = FRAME_DISPLAY_INFO (f);
  if (d->record && !d->record_failed && !d->snapshot_only
      && fprintf (d->record, "{\"seq\":%lu,\"event\":\"%s\"", d->events, event) < 0)
    d->record_failed = true;
  remote_stream_start (d, event);
}
static void finish (struct frame *f)
{
  part (f, "}\n");
  remote_stream_finish (FRAME_DISPLAY_INFO (f));
  if (!FRAME_DISPLAY_INFO (f)->snapshot_only) FRAME_DISPLAY_INFO (f)->events++;
}
static void rect (struct frame *f, int x, int y, int w, int h)
{ part (f, "[%lld,%lld,%lld,%lld]", U(x), U(y), U(w), U(h)); }
static void batch (struct frame *f)
{
  struct remote_display_info *d = FRAME_DISPLAY_INFO (f);
  if (d->batch_open || !d->record_ready) return;
  d->batch_open = true;
  start (f, "begin"); part (f, ",\"transaction\":%lu", ++d->transaction); finish (f);
}
static const char *cursor_kind (enum text_cursor_kinds kind)
{
  switch (kind)
    {
    case FILLED_BOX_CURSOR: return "box";
    case HOLLOW_BOX_CURSOR: return "hollow";
    case BAR_CURSOR: return "bar";
    case HBAR_CURSOR: return "hbar";
    default: return "none";
    }
}
/* Complete cursor state at presentation, including windows not updated in
   this pass.  Traverse the real window tree without retaining its pointers. */
static void cursor_state (struct frame *f, struct window *w, int *count)
{
  for (; w; w = NILP(w->next) ? NULL : XWINDOW(w->next))
    if (WINDOWP(w->contents)) cursor_state (f, XWINDOW(w->contents), count);
    else if (w->phys_cursor_on_p && w->phys_cursor_type != NO_CURSOR
             && w->current_matrix && w->phys_cursor.vpos >= 0
             && w->phys_cursor.vpos < w->current_matrix->nrows)
      {
        struct glyph_row *row = MATRIX_ROW(w->current_matrix,w->phys_cursor.vpos);
        struct glyph *g = row->enabled_p ? get_phys_cursor_glyph(w) : NULL;
        if (!g) continue;
        int saved_width=w->phys_cursor_width;
        int x,y,h; get_phys_cursor_geometry(w,row,g,&x,&y,&h);
        int width=w->phys_cursor_width;
        if(w->phys_cursor_type==BAR_CURSOR) width=saved_width;
        if(w->phys_cursor_type==HBAR_CURSOR)
          { y+=h-w->phys_cursor_height; h=w->phys_cursor_height; }
        w->phys_cursor_width=saved_width;
        if ((*count)++) part(f,",");
        part(f,"{\"kind\":\"%s\",\"rect\":",cursor_kind(w->phys_cursor_type));
        rect(f,x,y,width,h); part(f,"}");
      }
}
static void present (struct frame *f)
{
  struct remote_display_info *d = FRAME_DISPLAY_INFO(f);
  if (!d->batch_open) return;
  start(f,"present");
  part(f,",\"transaction\":%lu,\"presentation\":%lu,\"cursors\":[",
       d->transaction,++d->presentations);
  int count=0;
  if (WINDOWP(FRAME_ROOT_WINDOW(f))) cursor_state(f,XWINDOW(FRAME_ROOT_WINDOW(f)),&count);
  part(f,"]"); finish(f); d->batch_open=false;
}
void remote_flush (struct frame *f)
{
  struct remote_display_info *d=FRAME_DISPLAY_INFO(f);
  /* Cursor/clear callbacks can also run outside update_frame.  A flush then
     presents their accumulated damage.  A flush inside an update cannot. */
  if (!d->in_update) present(f);
  if (d->record && fflush(d->record)) d->record_failed=true;
}
void remote_begin (struct frame *f)
{ FRAME_DISPLAY_INFO(f)->in_update=true; batch(f); }
void remote_end (struct frame *f)
{ FRAME_DISPLAY_INFO(f)->in_update=false; present(f); remote_flush(f); }
static void font_resource (struct frame *f)
{
  start(f,"font");
  part(f,",\"id\":1,\"generation\":%lu,\"sha256\":\"" HASH "\","
       "\"face_index\":0,\"size\":16384,\"ascent\":15360,\"descent\":4096,"
       "\"line_height\":19456,\"column_width\":10240,\"antialias\":\"gray\","
       "\"hinting\":\"none\",\"hint_metrics\":false,\"embedded_bitmaps\":false",
       FRAME_DISPLAY_INFO(f)->generation);
  finish(f);
}
void remote_record_bootstrap (struct frame *f)
{
  start(f,"header"); part(f,",\"protocol\":\"remote-emacs-display\",\"version\":1,\"units\":1024"); finish(f);
  font_resource(f);
  start(f,"create-frame");
  part(f,",\"id\":1,\"width\":%lld,\"height\":%lld,\"scale\":%u,\"background\":%lu",
       U(FRAME_PIXEL_WIDTH(f)),U(FRAME_PIXEL_HEIGHT(f)),FRAME_DISPLAY_INFO(f)->scale,FRAME_BACKGROUND_PIXEL(f)); finish(f);
  remote_flush(f);
}
void remote_record_create (struct frame *f)
{
  struct remote_display_info *d=FRAME_DISPLAY_INFO(f);
  d->record_ready=true; d->generation=1;
  remote_record_bootstrap(f);
}
void remote_record_delete (struct frame *f)
{
  struct remote_display_info *d=FRAME_DISPLAY_INFO(f);
  if (!d->record_ready) return;
  remote_end(f);
  start(f,"delete-frame"); part(f,",\"id\":1"); finish(f);
  start(f,"retire-font"); part(f,",\"id\":1,\"generation\":%lu",d->generation); finish(f);
  remote_flush(f); d->record_ready=false;
}
void remote_record_invalidate (struct frame *f)
{
  struct remote_display_info *d=FRAME_DISPLAY_INFO(f);
  remote_end(f);
  start(f,"retire-font"); part(f,",\"id\":1,\"generation\":%lu",d->generation); finish(f);
  d->generation++; font_resource(f);
}
void remote_clear (struct frame *f)
{
  batch(f); start(f,"reset");
  part(f,",\"width\":%lld,\"height\":%lld,\"scale\":%u,\"background\":%lu",
       U(FRAME_PIXEL_WIDTH(f)),U(FRAME_PIXEL_HEIGHT(f)),FRAME_DISPLAY_INFO(f)->scale,FRAME_BACKGROUND_PIXEL(f)); finish(f);
}
void remote_record_resize (struct frame *f)
{
  if (!FRAME_DISPLAY_INFO(f)->record_ready) return;
  /* Resize discards the old raster.  Emacs regenerates its glyph matrices. */
  batch(f); start(f,"resize");
  part(f,",\"width\":%lld,\"height\":%lld,\"scale\":%u,\"background\":%lu",
       U(FRAME_PIXEL_WIDTH(f)),U(FRAME_PIXEL_HEIGHT(f)),FRAME_DISPLAY_INFO(f)->scale,FRAME_BACKGROUND_PIXEL(f)); finish(f);
  SET_FRAME_GARBAGED(f);
}
static void fill (struct frame *f, int x,int y,int w,int h,unsigned long color)
{
  if (w<=0 || h<=0) return;
  batch(f); start(f,"fill"); part(f,",\"rect\":"); rect(f,x,y,w,h);
  part(f,",\"color\":%lu",color); finish(f);
}
void remote_clear_area (struct frame *f,int x,int y,int w,int h)
{ fill(f,x,y,w,h,FRAME_BACKGROUND_PIXEL(f)); }
void remote_border (struct frame *f) {} /* Fixed zero-width internal border. */
static void unsupported (struct frame *f, const char *reason, int x,int y,int w,int h)
{
  batch(f); start(f,"unsupported"); part(f,",\"reason\":\"%s\",\"rect\":",reason);
  rect(f,x,y,w,h); finish(f);
}
/* Stretch glyphs represent tabs, end padding, or display substitutions.  Do
   not silently bless missing-font text which Emacs replaced by a stretch. */
static bool supported_stretch (struct glyph *g)
{
  int ch=0;
  if (BUFFERP(g->object))
    {
      struct buffer *b=XBUFFER(g->object);
      if (g->charpos>=BUF_BEGV(b) && g->charpos<BUF_ZV(b))
        ch=BUF_FETCH_CHAR_AS_MULTIBYTE(b,buf_charpos_to_bytepos(b,g->charpos));
    }
  else if (STRINGP(g->object) && g->charpos>=0 && g->charpos<SCHARS(g->object))
    ch=STRING_MULTIBYTE(g->object)
      ? STRING_CHAR(SDATA(g->object)+string_char_to_byte(g->object,g->charpos))
      : SREF(g->object,g->charpos);
  return ch==0 || ch==9 || ch==10 || ch==32;
}
void remote_draw (struct glyph_string *s)
{
  struct frame *f=s->f;
  prepare_face_for_display(f,s->face);
  bool stretch=s->first_glyph->type==STRETCH_GLYPH;
  if ((s->first_glyph->type!=CHAR_GLYPH && !stretch) || s->font_not_found_p)
    { unsupported(f,"glyph-kind-or-missing-font",s->x,s->y,s->width,s->height); return; }
  if (stretch && !supported_stretch(s->first_glyph))
    { unsupported(f,"stretch-substitution",s->x,s->y,s->width,s->height); return; }
  if (!stretch)
    for (int i=0;i<s->nchars;i++)
      if (s->first_glyph[i].u.ch>255 || s->first_glyph[i].resolved_level)
        { unsupported(f,"non-latin-1-or-bidi",s->x,s->y,s->width,s->height); return; }
  if (s->face->underline || s->face->overline_p || s->face->strike_through_p
      || s->face->box!=FACE_NO_BOX || s->stippled_p)
    { unsupported(f,"face-decoration",s->x,s->y,s->width,s->height); return; }
  if (FONT_WEIGHT_NAME_NUMERIC(s->face->lface[LFACE_WEIGHT_INDEX])!=80
      || FONT_SLANT_NAME_NUMERIC(s->face->lface[LFACE_SLANT_INDEX])!=100
      || FONT_WIDTH_NAME_NUMERIC(s->face->lface[LFACE_SWIDTH_INDEX])!=100
      || !FIXNUMP(s->face->lface[LFACE_HEIGHT_INDEX])
      || XFIXNUM(s->face->lface[LFACE_HEIGHT_INDEX])!=120
      || !STRINGP(s->face->lface[LFACE_FAMILY_INDEX])
      || strcmp(SSDATA(s->face->lface[LFACE_FAMILY_INDEX]),"DejaVu Sans Mono"))
    { unsupported(f,"font-style",s->x,s->y,s->width,s->height); return; }
  if (s->hl!=DRAW_NORMAL_TEXT && s->hl!=DRAW_CURSOR && s->hl!=DRAW_INVERSE_VIDEO)
    { unsupported(f,"highlight",s->x,s->y,s->width,s->height); return; }
  remote_font_draw(s,0,stretch ? 0 : s->nchars,s->x,s->ybase,!s->for_overlaps);
  s->background_filled_p=!s->for_overlaps;
}
int remote_font_draw (struct glyph_string *s,int from,int to,int x,int y,bool background)
{
  struct frame *f=s->f;
  struct remote_display_info *d=FRAME_DISPLAY_INFO(f);
  NativeRectangle clips[2]; int n=get_glyph_string_clip_rects(s,clips,2);
  unsigned long fg=s->face->foreground, bg=s->face->background;
  /* The experiment's fixed cursor color is black, with the face background
     as cursor text; avoid an invisible cursor on a black background. */
  if (s->hl==DRAW_CURSOR)
    { bg=0; fg=s->face->background; if (fg==bg) bg=s->face->foreground; }
  batch(f); start(f,"run");
  part(f,",\"font\":1,\"generation\":%lu,\"foreground\":%lu,\"background\":%lu,"
       "\"paint_background\":%s,\"rect\":",d->generation,fg,bg,background?"true":"false");
  rect(f,s->x,s->y,s->background_width,s->height); part(f,",\"clips\":[");
  for(int i=0;i<n;i++)
    { if(i)part(f,","); rect(f,clips[i].x,clips[i].y,clips[i].width,clips[i].height); }
  part(f,"],\"glyphs\":[");
  for(int i=from;i<to;i++)
    {
      struct glyph *g=s->first_glyph+i;
      if(i!=from)part(f,",");
      part(f,"{\"id\":%u,\"x\":%lld,\"y\":%lld,\"advance\":%lld,\"character\":%u}",
           s->char2b[i],U(x),U(y),U(g->pixel_width),g->u.ch);
      x+=g->pixel_width; d->glyphs++;
    }
  part(f,"]"); finish(f); return to-from;
}
void remote_scroll (struct window *w,struct run *r)
{
  struct frame *f=WINDOW_XFRAME(w);
  int x,y,width,height; window_box(w,ANY_AREA,&x,&y,&width,&height);
  int from=WINDOW_TO_FRAME_PIXEL_Y(w,r->current_y);
  int to=WINDOW_TO_FRAME_PIXEL_Y(w,r->desired_y);
  height=min(r->height,y+height-max(from,to));
  if(height<=0 || width<=0) return;
  block_input(); gui_clear_cursor(w);
  batch(f); start(f,"scroll"); part(f,",\"src\":"); rect(f,x,from,width,height);
  part(f,",\"dst\":[%lld,%lld]",U(x),U(to)); finish(f);
  unblock_input();
}
void remote_cursor (struct window *w,struct glyph_row *row,int x,int y,
                    enum text_cursor_kinds type,int width,bool on,bool active)
{
  struct frame *f=WINDOW_XFRAME(w);
  w->phys_cursor_type=on?type:NO_CURSOR; w->phys_cursor_on_p=on;
  struct glyph *g=get_phys_cursor_glyph(w);
  if(!g) return;
  /* A tab/stretch has no ink.  Restore its real background first, then
     paint only the cursor rectangle chosen by get_phys_cursor_geometry.
     DRAW_CURSOR on the entire stretch would incorrectly fill the tab. */
  if(on && type==FILLED_BOX_CURSOR && g->type==STRETCH_GLYPH)
    draw_phys_cursor_glyph(w,row,DRAW_NORMAL_TEXT);
  int h; get_phys_cursor_geometry(w,row,g,&x,&y,&h);
  int wd=w->phys_cursor_width;
  if(type==BAR_CURSOR) wd=min(wd,width<0?1:width);
  if(type==HBAR_CURSOR) { int bh=min(h,width<0?1:width); y+=h-bh; h=bh; }
  if(type==BAR_CURSOR) w->phys_cursor_width=wd;
  if(type==HBAR_CURSOR) w->phys_cursor_height=h;
  int bx,by,bw,bh; window_box(w,TEXT_AREA,&bx,&by,&bw,&bh);
  struct face *face=FACE_FROM_ID_OR_NULL(f,g->face_id);
  unsigned long color=face && face->background==0 ? face->foreground : 0;
  batch(f); start(f,"cursor"); part(f,",\"on\":%s,\"kind\":\"%s\",\"rect\":",on?"true":"false",cursor_kind(type));
  rect(f,x,y,wd,h); part(f,",\"clip\":"); rect(f,bx,by,bw,bh); part(f,",\"color\":%lu",color); finish(f);
  if(on && type==FILLED_BOX_CURSOR && g->type!=STRETCH_GLYPH) draw_phys_cursor_glyph(w,row,DRAW_CURSOR);
}
void remote_vertical (struct window *w,int x,int y0,int y1)
{
  struct frame *f=WINDOW_XFRAME(w);
  struct face *face=FACE_FROM_ID_OR_NULL(f,VERTICAL_BORDER_FACE_ID);
  fill(f,x,y0,1,y1-y0+1,face?face->foreground:FRAME_FOREGROUND_PIXEL(f));
}
void remote_divider (struct window *w,int x0,int x1,int y0,int y1)
{ unsupported(WINDOW_XFRAME(w),"window-divider",x0,y0,x1-x0,y1-y0); }
void remote_fringe (struct window *w,struct glyph_row *row,struct draw_fringe_bitmap_params *p)
{ unsupported(WINDOW_XFRAME(w),"fringe",0,0,0,0); }
