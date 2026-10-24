/* Native headless remote-display experiment.  GPL-3.0-or-later.
   This records authoritative Emacs geometry; it is not an interactive GUI. */
#include <config.h>
#include <math.h>
#include <stdarg.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "lisp.h"
#include "remoteterm.h"
#include "termhooks.h"
#include "keyboard.h"
#include "window.h"
#include "buffer.h"
#include "fontset.h"
#include "blockinput.h"
#include "sha256.h"
#include "pdumper.h"

#define REMOTE_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
#define REMOTE_HASH "a54dca07c76d6289e717e75e0a58c0128f6d7269ef3faf76417c9d7d3bba37ab"
struct remote_display_info *x_display_list;
static Lisp_Object remote_display_name;
static int remote_font_draw (struct glyph_string *, int, int, int, int, bool);
static struct font_driver remote_font_driver;
static frame_parm_handler remote_parm_handlers[51]; /* frame.c, this revision. */

struct remote_font {
  struct font font;
  FT_Library library;
  FT_Face face;
  unsigned char *bytes;
};

/* All retained data is owned.  No glyph, face, window or Lisp pointer is
   written to the recorder.  Failures are sticky, checked outside redisplay. */
static void remote_record (struct frame *, const char *, ...) ATTRIBUTE_FORMAT_PRINTF (2, 3);

static void
remote_record (struct frame *f, const char *format, ...)
{
  struct remote_display_info *d = FRAME_DISPLAY_INFO (f);
  if (!d->record || d->record_failed) return;
  va_list ap;
  va_start (ap, format);
  if (vfprintf (d->record, format, ap) < 0 || fputc ('\n', d->record) == EOF)
    d->record_failed = true;
  va_end (ap);
  d->events++;
}

static Lisp_Object remote_cache (struct frame *f) { return FRAME_DISPLAY_INFO (f)->name_list_element; }
static Lisp_Object
remote_match (struct frame *f, Lisp_Object spec)
{
  Lisp_Object e = font_make_entity ();
  ASET (e, FONT_TYPE_INDEX, Qremote);
  ASET (e, FONT_FOUNDRY_INDEX, intern_c_string ("dejavu"));
  ASET (e, FONT_FAMILY_INDEX, intern_c_string ("DejaVu Sans Mono"));
  ASET (e, FONT_REGISTRY_INDEX, Qiso10646_1);
  ASET (e, FONT_SIZE_INDEX, make_fixnum (16));
  ASET (e, FONT_DPI_INDEX, make_fixnum (96));
  ASET (e, FONT_AVGWIDTH_INDEX, make_fixnum (100));
  ASET (e, FONT_SPACING_INDEX, make_fixnum (FONT_SPACING_MONO));
  FONT_SET_STYLE (e, FONT_WEIGHT_INDEX, Qnormal);
  FONT_SET_STYLE (e, FONT_SLANT_INDEX, Qnormal);
  FONT_SET_STYLE (e, FONT_WIDTH_INDEX, Qnormal);
  return e;
}
static Lisp_Object remote_list (struct frame *f, Lisp_Object spec)
{ return list1 (remote_match (f, spec)); }

/* Hash and parse the same bytes, including when the file is replaced. */
static bool
remote_load_font (struct remote_font *rf)
{
  FILE *fp = fopen (REMOTE_FONT, "rb");
  if (!fp) return false;
  const size_t limit = 4 * 1024 * 1024;
  rf->bytes = xmalloc (limit);
  size_t n = fread (rf->bytes, 1, limit, fp);
  bool ok = !ferror (fp) && feof (fp) && n > 0;
  fclose (fp);
  unsigned char digest[32];
  char hex[65];
  sha256_buffer ((char *) rf->bytes, n, digest);
  for (int i = 0; i < 32; i++) sprintf (hex + 2*i, "%02x", digest[i]);
  if (!ok || strcmp (hex, REMOTE_HASH)) return false;
  return !FT_Init_FreeType (&rf->library)
    && !FT_New_Memory_Face (rf->library, rf->bytes, n, 0, &rf->face)
    && !FT_Set_Pixel_Sizes (rf->face, 0, 16);
}
static void
remote_close_font (struct font *font)
{
  struct remote_font *rf = (struct remote_font *) font;
  if (rf->face) FT_Done_Face (rf->face);
  if (rf->library) FT_Done_FreeType (rf->library);
  xfree (rf->bytes);
  rf->face = NULL; rf->library = NULL; rf->bytes = NULL;
}
static Lisp_Object
remote_open_font (struct frame *f, Lisp_Object entity, int size)
{
  if (size != 16) return Qnil;
  Lisp_Object obj = font_make_object (VECSIZE (struct remote_font), entity, size);
  struct remote_font *rf = (struct remote_font *) XFONT_OBJECT (obj);
  struct font *font = &rf->font;
  memset ((char *)font + offsetof (struct font, min_width), 0,
          sizeof *rf - offsetof (struct font, min_width));
  ASET (obj, FONT_TYPE_INDEX, Qremote);
  font->driver = &remote_font_driver;
  if (!remote_load_font (rf))
    { remote_close_font (font); return Qnil; }
  font->pixel_size = 16;
  font->ascent = lround (rf->face->ascender * 16.0 / rf->face->units_per_EM);
  font->descent = lround (-rf->face->descender * 16.0 / rf->face->units_per_EM);
  font->height = font->ascent + font->descent;
  FT_Load_Char (rf->face, 'M', FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP);
  font->average_width = font->space_width = font->max_width
    = lround (rf->face->glyph->advance.x / 64.0);
  font->min_width = 0; /* combining glyphs can have zero advance. */
  font->underline_thickness = 1; font->underline_position = 2;
  font->encoding_charset = font->repertory_charset = -1;
  ASET (obj, FONT_FILE_INDEX, build_string (REMOTE_FONT));
  ASET (obj, FONT_NAME_INDEX,
        build_string ("-dejavu-DejaVu Sans Mono-normal-r-normal--16-120-96-96-m-100-iso10646-1"));
  ASET (obj, FONT_FULLNAME_INDEX, AREF (obj, FONT_NAME_INDEX));
  return obj;
}
static unsigned
remote_encode (struct font *font, int c)
{
  unsigned g = FT_Get_Char_Index (((struct remote_font *) font)->face, c);
  return g ? g : FONT_INVALID_CODE;
}
static int remote_has_char (Lisp_Object font, int c)
{ return FONT_OBJECT_P (font) ? remote_encode (XFONT_OBJECT (font), c) != FONT_INVALID_CODE : -1; }
static void
remote_extents (struct font *font, const unsigned *codes, int count,
                struct font_metrics *m)
{
  FT_Face face = ((struct remote_font *) font)->face;
  memset (m, 0, sizeof *m);
  int width = 0;
  for (int i = 0; i < count; i++)
    {
      if (FT_Load_Glyph (face, codes[i], FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP))
        continue;
      FT_Glyph_Metrics g = face->glyph->metrics;
      int left = floor (g.horiBearingX / 64.0);
      int right = ceil ((g.horiBearingX + g.width) / 64.0);
      m->lbearing = i ? min (m->lbearing, width + left) : left;
      m->rbearing = max (m->rbearing, width + right);
      m->ascent = max (m->ascent, (int) ceil (g.horiBearingY / 64.0));
      m->descent = max (m->descent, (int) ceil ((g.height - g.horiBearingY) / 64.0));
      width += lround (face->glyph->advance.x / 64.0);
    }
  m->width = width;
}
static struct font_driver remote_font_driver = {
  .get_cache = remote_cache, .list = remote_list, .match = remote_match,
  .open_font = remote_open_font, .close_font = remote_close_font,
  .has_char = remote_has_char, .encode_char = remote_encode,
  .text_extents = remote_extents, .draw = remote_font_draw
};

static bool
remote_color (struct frame *f, const char *name, Emacs_Color *c, bool a, bool b)
{
  if (!strcmp (name, "black")) c->red = c->green = c->blue = 0;
  else if (!strcmp (name, "white")) c->red = c->green = c->blue = 65535;
  else if (!parse_color_spec (name, &c->red, &c->green, &c->blue)) return false;
  c->pixel = ((unsigned long)(c->red >> 8) << 16)
    | ((c->green >> 8) << 8) | (c->blue >> 8);
  return true;
}
static void remote_background (struct frame *f, Emacs_Color *c)
{ remote_color (f, "white", c, false, false); }
static Lisp_Object
remote_new_font (struct frame *f, Lisp_Object obj, int fontset)
{
  FRAME_FONT (f) = XFONT_OBJECT (obj);
  FRAME_FONTSET (f) = fontset < 0 ? fontset_from_font (obj) : fontset;
  FRAME_COLUMN_WIDTH (f) = FRAME_FONT (f)->average_width;
  FRAME_LINE_HEIGHT (f) = FRAME_FONT (f)->height;
  return obj;
}
static void
remote_flush (struct frame *f)
{
  struct remote_display_info *d = FRAME_DISPLAY_INFO (f);
  if (d->record && fflush (d->record)) d->record_failed = true;
}
static void remote_begin (struct frame *f)
{ remote_record (f, "{\"event\":\"begin\"}"); }
static void remote_end (struct frame *f)
{ remote_record (f, "{\"event\":\"end\"}"); remote_flush (f); }
static void
remote_clear_area (struct frame *f, int x, int y, int w, int h)
{ remote_record (f, "{\"event\":\"clear\",\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}", x,y,w,h); }
static void remote_clear (struct frame *f)
{ remote_clear_area (f, 0, 0, FRAME_PIXEL_WIDTH (f), FRAME_PIXEL_HEIGHT (f)); }
static void remote_border (struct frame *f) {}
static void
remote_draw (struct glyph_string *s)
{
  if (s->first_glyph->type != CHAR_GLYPH || s->font_not_found_p)
    {
      remote_record (s->f, "{\"event\":\"unsupported\",\"glyph_type\":%d,\"missing_font\":%s,\"x\":%d,\"y\":%d,\"width\":%d}",
                     s->first_glyph->type, s->font_not_found_p ? "true" : "false", s->x, s->y, s->width);
      return;
    }
  for (int i = 0; i < s->nchars; ++i)
    if (s->first_glyph[i].u.ch > 255 || s->first_glyph[i].resolved_level)
      {
        remote_record (s->f, "{\"event\":\"unsupported-text\",\"character\":%u,\"bidi_level\":%u}",
                       s->first_glyph[i].u.ch, (unsigned) s->first_glyph[i].resolved_level);
        return;
      }
  if (s->face->underline || s->face->overline_p || s->face->strike_through_p
      || s->face->box != FACE_NO_BOX || s->stippled_p)
    remote_record (s->f, "{\"event\":\"unsupported-face-decoration\"}");
  NativeRectangle clips[2];
  int n = get_glyph_string_clip_rects (s, clips, 2);
  int x = s->x;
  remote_record (s->f, "{\"event\":\"run\",\"x\":%d,\"y\":%d,\"width\":%d,\"background_width\":%d,\"height\":%d,\"foreground\":%lu,\"background\":%lu,\"clip_count\":%d}",
                 x,s->y,s->width,s->background_width,s->height,s->face->foreground,s->face->background,n);
  for (int i=0; i<n; i++)
    remote_record (s->f, "{\"event\":\"clip\",\"x\":%d,\"y\":%d,\"width\":%u,\"height\":%u}",
                   clips[i].x, clips[i].y, clips[i].width, clips[i].height);
  remote_font_draw (s,0,s->nchars,x,s->ybase,true);
}
static int
remote_font_draw (struct glyph_string *s, int from, int to, int x, int y,
                  bool background)
{
  struct remote_display_info *d = FRAME_DISPLAY_INFO (s->f);
  for (int i = from; i < to; i++)
    {
      struct glyph *g = s->first_glyph + i;
      remote_record (s->f, "{\"event\":\"glyph\",\"font\":1,\"glyph_id\":%u,\"character\":%u,\"x\":%d,\"baseline\":%d,\"advance\":%d,\"x_1024\":%lld,\"baseline_1024\":%lld,\"advance_1024\":%lld}",
                     s->char2b[i], g->u.ch, x,y,g->pixel_width,
                     (long long)x*1024,(long long)y*1024,(long long)g->pixel_width*1024);
      x += g->pixel_width;
      d->glyphs++;
    }
  return to-from;
}

static void remote_scroll (struct window *w, struct run *r)
{ remote_record (XFRAME (WINDOW_FRAME (w)), "{\"event\":\"scroll\",\"from\":%d,\"to\":%d,\"height\":%d}", r->current_y,r->desired_y,r->height); }
static void remote_pointer (struct frame *f, Emacs_Cursor c) {}
static void
remote_cursor (struct window *w, struct glyph_row *row, int x, int y,
               enum text_cursor_kinds type, int width, bool on, bool active)
{
  w->phys_cursor_type = type;
  w->phys_cursor_on_p = on;
  remote_record (XFRAME (WINDOW_FRAME (w)), "{\"event\":\"cursor\",\"x\":%d,\"y\":%d,\"type\":%d,\"width\":%d,\"on\":%s}", x,y,type,width,on?"true":"false");
}
static void remote_vertical (struct window *w, int x, int y0, int y1)
{ remote_record (XFRAME (WINDOW_FRAME (w)), "{\"event\":\"border\",\"x\":%d,\"y0\":%d,\"y1\":%d}", x,y0,y1); }
static void remote_divider (struct window *w,int x0,int x1,int y0,int y1)
{ remote_record (XFRAME (WINDOW_FRAME (w)), "{\"event\":\"divider\",\"x0\":%d,\"x1\":%d,\"y0\":%d,\"y1\":%d}",x0,x1,y0,y1); }
static void remote_fringe (struct window *w, struct glyph_row *row, struct draw_fringe_bitmap_params *p)
{ remote_record (XFRAME (WINDOW_FRAME (w)), "{\"event\":\"unsupported-fringe\"}"); }
static void remote_fringe_define (int n, unsigned short *bits, int h, int w) {}
static void remote_fringe_destroy (int n) {}
static void remote_after_line (struct window *w, struct glyph_row *r) {}
static void remote_window_end (struct window *w, bool on, bool mouse)
{
  block_input ();
  display_and_set_cursor (w, on, w->cursor.hpos, w->cursor.vpos, w->cursor.x, w->cursor.y);
  unblock_input ();
}
static void remote_hourglass (struct frame *f) {}
static struct redisplay_interface remote_redisplay = {
  .frame_parm_handlers = remote_parm_handlers,
  .produce_glyphs = gui_produce_glyphs, .write_glyphs = gui_write_glyphs,
  .insert_glyphs = gui_insert_glyphs, .clear_end_of_line = gui_clear_end_of_line,
  .scroll_run_hook = remote_scroll, .after_update_window_line_hook = remote_after_line,
  .update_window_end_hook = remote_window_end, .flush_display = remote_flush,
  .clear_window_mouse_face = gui_clear_window_mouse_face,
  .get_glyph_overhangs = gui_get_glyph_overhangs,
  .fix_overlapping_area = gui_fix_overlapping_area,
  .draw_fringe_bitmap = remote_fringe, .define_fringe_bitmap = remote_fringe_define,
  .destroy_fringe_bitmap = remote_fringe_destroy, .draw_glyph_string = remote_draw,
  .define_frame_cursor = remote_pointer, .clear_frame_area = remote_clear_area,
  .clear_under_internal_border = remote_border, .draw_window_cursor = remote_cursor,
  .draw_vertical_window_border = remote_vertical, .draw_window_divider = remote_divider,
  .show_hourglass = remote_hourglass, .hide_hourglass = remote_hourglass
};
static void
remote_delete_frame (struct frame *f)
{
  remote_record (f, "{\"event\":\"delete-frame\"}");
  free_frame_faces (f);
  xfree (FRAME_OUTPUT_DATA (f));
  FRAME_OUTPUT_DATA (f) = NULL;
}
static void
remote_delete_terminal (struct terminal *t)
{
  struct remote_display_info *d = t->display_info.remote;
  delete_terminal (t); /* First delete live frames, then keyboard/coding state. */
  if (d->record) fclose (d->record);
  x_display_list = NULL;
  remote_display_name = Qnil;
  xfree (d);
  t->display_info.remote = NULL;
}
static void
remote_unwind_frame (Lisp_Object obj)
{
  struct frame *f = XFRAME (obj);
  if (FRAME_LIVE_P (f) && NILP (Fmemq (obj,Vframe_list)))
    {
      struct terminal *t = f->terminal;
      font_update_drivers (f,Qnil);
      free_font_driver_list (f);
      remote_delete_frame (f);
      free_glyphs (f);
      f->terminal = NULL;
      remote_delete_terminal (t);
    }
}
static void remote_visibility (struct frame *f, bool visible)
{ SET_FRAME_VISIBLE (f, visible); if (visible) SET_FRAME_GARBAGED (f); }
static Lisp_Object remote_focus (struct frame *f)
{ return Qnil; /* No input source and thus no external focus. */ }
static const char *remote_resource (void *db, const char *key, const char *cl)
{ return NULL; }
static void remote_name (struct frame *f,Lisp_Object n,Lisp_Object old) {}
static void remote_size (struct frame *f, bool gravity,int w,int h)
{
  change_frame_size (f,w,h,false,false,false);
  remote_record (f,"{\"event\":\"resize\",\"width\":%d,\"height\":%d}",w,h);
}
static void remote_offset (struct frame *f, int x, int y, int gravity)
{ error ("Remote recorder has no desktop position"); }
static void remote_iconify (struct frame *f)
{ error ("Remote recorder cannot be iconified"); }
static Lisp_Object remote_menu (struct frame *f, int x, int y, int flags,
                                 Lisp_Object title, const char **err)
{ *err = "Remote recorder has no menus"; return Qnil; }
static Lisp_Object remote_dialog (struct frame *f, Lisp_Object h, Lisp_Object c)
{ error ("Remote recorder has no dialogs"); }
void
remote_validate_frame_parameters (struct frame *f, Lisp_Object alist)
{
  Lisp_Object item, tail;
  XSETFRAME (item,f);
  FOR_EACH_TAIL (alist)
    {
      tail = XCAR (alist);
      CHECK_CONS (tail);
      Lisp_Object key = XCAR(tail), value = XCDR(tail);
      Lisp_Object index = Fget(key,Qx_frame_parameter);
      if ((EQ(key,Qleft) || EQ(key,Qtop)
           || (FIXNATP(index) && !EQ(key,Qfont) && !EQ(key,Qline_spacing)))
          && NILP(Fequal(value,Fframe_parameter(item,key))))
        error ("Frame parameter %s unsupported by remote recorder", SSDATA(SYMBOL_NAME(key)));
    }
}
static void remote_unsupported_param (struct frame *f,Lisp_Object n,Lisp_Object o)
{ if (!EQ(n,o)) error ("Frame parameter unsupported by remote recorder"); }

DEFUN ("remote-create-frame", Fremote_create_frame, Sremote_create_frame, 1, 1, 0,
       doc: /* Create a native headless recorder frame, writing JSON lines to FILE.
Only one remote frame may exist.  Requires the pinned DejaVu Sans Mono font.
Use `delete-frame' or `delete-terminal' for normal lifecycle cleanup.
This experimental frame has no external input source.  */)
  (Lisp_Object file)
{
  CHECK_STRING (file);
  if (x_display_list) error ("Only one remote recorder frame is supported");
  file = Fexpand_file_name (file,Qnil);
  /* Preflight before constructing a frame; never abort inside face setup
     because the resource was absent or mismatched. */
  struct remote_font probe = {0};
  bool valid = remote_load_font (&probe);
  remote_close_font (&probe.font);
  if (!valid) error ("Remote font missing or SHA-256 mismatch: " REMOTE_FONT);
  FILE *out = fopen (SSDATA (file), "wx");
  if (!out) report_file_error ("Creating remote recorder",file);
  struct remote_display_info *d = xzalloc (sizeof *d);
  d->record = out; d->resx = d->resy = 96; d->n_planes = 24;
  d->smallest_char_width = 10; d->smallest_font_height = 19;
  reset_mouse_highlight (&d->mouse_highlight);
  struct terminal *t = create_terminal (output_remote,&remote_redisplay);
  t->name = xstrdup ("remote-recorder"); t->display_info.remote = d;
  d->terminal = t;
  t->kboard = allocate_kboard (Qremote); t->kboard->reference_count++;
  t->delete_frame_hook = remote_delete_frame;
  t->delete_terminal_hook = remote_delete_terminal;
  t->set_new_font_hook = remote_new_font; t->defined_color_hook = remote_color;
  t->query_frame_background_color = remote_background;
  t->frame_visible_invisible_hook = remote_visibility;
  t->get_focus_frame = remote_focus; t->get_string_resource_hook = remote_resource;
  t->set_frame_offset_hook = remote_offset;
  t->iconify_frame_hook = remote_iconify;
  t->menu_show_hook = remote_menu; t->popup_dialog_hook = remote_dialog;
  t->set_window_size_hook = remote_size; t->implicit_set_name_hook = remote_name;
  t->clear_frame_hook = remote_clear;
  t->update_begin_hook = remote_begin; t->update_end_hook = remote_end;
  struct frame *f = make_frame (true);
  Lisp_Object frame; XSETFRAME (frame,f);
  f->output_method = output_remote; f->terminal = t;
  f->output_data.remote = xzalloc (sizeof *f->output_data.remote);
  FRAME_DISPLAY_INFO (f) = d; FRAME_FONTSET (f) = -1;
  specpdl_ref count = SPECPDL_INDEX ();
  record_unwind_protect (remote_unwind_frame,frame);
  x_display_list = d;
  remote_display_name = Fcons (build_string ("remote-recorder"),Qnil);
  d->name_list_element = remote_display_name;
  fset_name (f,build_string ("Remote recorder"));
  FRAME_FOREGROUND_PIXEL (f) = 0; FRAME_BACKGROUND_PIXEL (f) = 0xffffff;
  f->left_fringe_width = f->right_fringe_width = 0;
  f->vertical_scroll_bar_type = vertical_scroll_bar_none;
  f->horizontal_scroll_bars = false;
  f->internal_border_width = 0;
  register_font_driver (&remote_font_driver,f);
  font_update_drivers (f,list1(Qremote));
  store_frame_param (f,Qfont_backend,list1(Qremote));
  Lisp_Object spec = font_spec_from_name (build_string ("DejaVu Sans Mono-12"));
  ASET (spec,FONT_SIZE_INDEX,make_fixnum(16));
  Lisp_Object font = font_open_by_spec (f,spec);
  if (NILP(font)) error ("Cannot open remote font");
  gui_set_font (f,font,Qnil);
  store_frame_param (f,Qforeground_color,build_string ("black"));
  store_frame_param (f,Qbackground_color,build_string ("white"));
  store_frame_param (f,Qmenu_bar_lines,make_fixnum(0));
  store_frame_param (f,Qtool_bar_lines,make_fixnum(0));
  store_frame_param (f,Qtab_bar_lines,make_fixnum(0));
  store_frame_param (f,Qvertical_scroll_bars,Qnil);
  store_frame_param (f,Qhorizontal_scroll_bars,Qnil);
  init_frame_faces (f);
  adjust_frame_size (f,800,475,5,true,Qremote);
  t->reference_count++;
  Vframe_list = Fcons (frame,Vframe_list);
  SET_FRAME_VISIBLE (f,true); SET_FRAME_GARBAGED (f);
  remote_record (f,"{\"event\":\"create-frame\",\"units\":1024,\"font\":1,\"font_sha256\":\"" REMOTE_HASH "\",\"size\":16,\"ascent\":%d,\"descent\":%d,\"column_width\":%d,\"line_height\":%d,\"width\":%d,\"height\":%d}",
                 FRAME_FONT(f)->ascent,FRAME_FONT(f)->descent,FRAME_COLUMN_WIDTH(f),FRAME_LINE_HEIGHT(f),FRAME_PIXEL_WIDTH(f),FRAME_PIXEL_HEIGHT(f));
  remote_flush (f);
  return unbind_to (count,frame);
}
DEFUN ("remote-recorder-status", Fremote_recorder_status, Sremote_recorder_status, 1, 1, 0,
       doc: /* Return [EVENTS GLYPHS FAILED] for live remote FRAME.  */)
  (Lisp_Object frame)
{
  struct frame *f = decode_live_frame (frame);
  if (!FRAME_REMOTE_P(f)) error ("Not a remote frame");
  remote_flush(f);
  struct remote_display_info *d = FRAME_DISPLAY_INFO(f);
  Lisp_Object values[] = {make_fixnum(d->events), make_fixnum(d->glyphs), d->record_failed?Qt:Qnil};
  return Fvector (3,values);
}
DEFUN ("xw-display-color-p", Fxw_display_color_p, Sxw_display_color_p, 0, 1, 0,
       doc: /* Whether TERMINAL is a remote RGB recorder.  */)
  (Lisp_Object terminal)
{ return decode_live_terminal (terminal)->type == output_remote ? Qt : Qnil; }
DEFUN ("x-display-grayscale-p", Fx_display_grayscale_p, Sx_display_grayscale_p, 0, 1, 0,
       doc: /* Whether TERMINAL is a remote RGB recorder.  */)
  (Lisp_Object terminal)
{ return Fxw_display_color_p (terminal); }
void frame_set_mouse_pixel_position (struct frame *f, int x, int y)
{ error ("Remote recorder has no pointing device"); }
char *get_keysym_name (int key) { return NULL; }
Display_Info *check_x_display_info (Lisp_Object object)
{
  struct terminal *t = decode_live_terminal (object);
  if (t->type != output_remote) error ("Not a remote display");
  return t->display_info.remote;
}
static void
remote_init_runtime (void)
{
  remote_font_driver.type = Qremote;
  register_font_driver (&remote_font_driver,NULL);
  for (int i=0;i<ARRAYELTS(remote_parm_handlers);i++)
    remote_parm_handlers[i] = remote_unsupported_param;
  remote_parm_handlers[7] = gui_set_font;
  remote_parm_handlers[30] = gui_set_line_spacing;
}
void
syms_of_remote (void)
{
  DEFSYM (Qremote,"remote");
  remote_display_name = Qnil; staticpro (&remote_display_name);
  pdumper_do_now_and_after_load (remote_init_runtime);
  defsubr (&Sxw_display_color_p); defsubr (&Sx_display_grayscale_p);
  defsubr (&Sremote_create_frame); defsubr (&Sremote_recorder_status);
  Fprovide (intern_c_string ("remote-display"),Qnil);
}
