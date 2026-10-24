/* Private recorder API; no transport or layout implementation. */
#ifndef EMACS_REMOTERECORD_H
#define EMACS_REMOTERECORD_H
extern void remote_record_bootstrap (struct frame *);
extern void remote_record_create (struct frame *);
extern void remote_record_delete (struct frame *);
extern void remote_record_resize (struct frame *);
extern void remote_record_invalidate (struct frame *);
extern void remote_begin (struct frame *);
extern void remote_end (struct frame *);
extern void remote_flush (struct frame *);
extern void remote_clear (struct frame *);
extern void remote_clear_area (struct frame *, int, int, int, int);
extern void remote_border (struct frame *);
extern void remote_draw (struct glyph_string *);
extern int remote_font_draw (struct glyph_string *, int, int, int, int, bool);
extern void remote_scroll (struct window *, struct run *);
extern void remote_cursor (struct window *, struct glyph_row *, int, int,
                           enum text_cursor_kinds, int, bool, bool);
extern void remote_vertical (struct window *, int, int, int);
extern void remote_divider (struct window *, int, int, int, int);
extern void remote_fringe (struct window *, struct glyph_row *,
                           struct draw_fringe_bitmap_params *);
#endif
