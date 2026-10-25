/* Native headless remote-display experiment.  GPL-3.0-or-later.
   Each native frame owns a terminal, keyboard and independent display state. */
#include <config.h>
#include <math.h>
#include <stdarg.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include "lisp.h"
#include "remoteterm.h"
#include "remoterecord.h"
#include "remoteinput.h"
#include "remotestream.h"
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
static struct font_driver remote_font_driver;
static frame_parm_handler remote_parm_handlers[51]; /* frame.c, this revision. */

struct remote_font {
  struct font font;
  FT_Library library;
  FT_Face face;
  unsigned char *bytes;
};

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
static void remote_pointer (struct frame *f, Emacs_Cursor c) {}
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
  remote_input_close (FRAME_DISPLAY_INFO (f));
  remote_stream_close (FRAME_DISPLAY_INFO (f));
  remote_record_delete (f);
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
  struct remote_display_info **link = &x_display_list;
  while (*link && *link != d) link = &(*link)->next;
  eassert (*link == d);
  *link = d->next;
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
{
  Lisp_Object frame = Qnil;
  if (FRAME_DISPLAY_INFO (f)->highlight_frame) XSETFRAME (frame, f);
  return frame;
}
static const char *remote_resource (void *db, const char *key, const char *cl)
{ return NULL; }
static void remote_name (struct frame *f,Lisp_Object n,Lisp_Object old) {}
static void remote_size (struct frame *f, bool gravity,int w,int h)
{
  change_frame_size (f,w,h,false,false,false);
  remote_record_resize (f);
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
Each frame owns a distinct terminal and keyboard.
Requires the pinned DejaVu Sans Mono font.
Use `delete-frame' or `delete-terminal' for normal lifecycle cleanup.
FILE may be nil to disable diagnostic disk writes.  Use the separate
`remote-input-listen' and `remote-display-listen' endpoints to attach a client.  */)
  (Lisp_Object file)
{
  if (!NILP (file)) CHECK_STRING (file);
  if (!NILP (file)) file = Fexpand_file_name (file,Qnil);
  /* Preflight before constructing a frame; never abort inside face setup
     because the resource was absent or mismatched. */
  struct remote_font probe = {0};
  bool valid = remote_load_font (&probe);
  remote_close_font (&probe.font);
  if (!valid) error ("Remote font missing or SHA-256 mismatch: " REMOTE_FONT);
  FILE *out = NILP (file) ? NULL : fopen (SSDATA (file), "wx");
  if (!NILP (file) && !out) report_file_error ("Creating remote recorder",file);
  struct remote_display_info *d = xzalloc (sizeof *d);
  d->scale = 1;
  d->record = out; d->resx = d->resy = 96; d->n_planes = 24;
  d->smallest_char_width = 10; d->smallest_font_height = 19;
  reset_mouse_highlight (&d->mouse_highlight);
  struct terminal *t = create_terminal (output_remote,&remote_redisplay);
  char name[64];
  snprintf (name, sizeof name, "remote-native-%d", t->id);
  t->name = xstrdup (name); t->display_info.remote = d;
  d->terminal = t;
  t->kboard = allocate_kboard (Qremote); t->kboard->reference_count++;
  t->read_socket_hook = remote_read_socket;
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
  d->next = x_display_list;
  x_display_list = d;
  /* compact_font_caches marks this through TERMINAL_FONT_CACHE for every
     live terminal.  Never share a cache cell between native displays. */
  d->name_list_element = Fcons (build_string (name), Qnil);
  fset_name (f, build_string (name));
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
  remote_record_create (f);
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
DEFUN ("remote-recorder-checkpoint", Fremote_recorder_checkpoint,
       Sremote_recorder_checkpoint, 1, 1, 0,
       doc: /* Invalidate the recording font epoch and request a full redraw.
Call `redisplay' afterwards.  This does not create a second layout engine.  */)
  (Lisp_Object frame)
{
  struct frame *f = decode_live_frame (frame);
  if (!FRAME_REMOTE_P (f)) error ("Not a remote frame");
  remote_record_invalidate (f);
  Fredraw_frame (frame);
  return Qnil;
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
  syms_of_remote_input ();
  syms_of_remote_stream ();
  DEFSYM (Qremote,"remote");
  pdumper_do_now_and_after_load (remote_init_runtime);
  defsubr (&Sxw_display_color_p); defsubr (&Sx_display_grayscale_p);
  defsubr (&Sremote_create_frame); defsubr (&Sremote_recorder_status);
  defsubr (&Sremote_recorder_checkpoint);
  Fprovide (intern_c_string ("remote-display"),Qnil);
}
