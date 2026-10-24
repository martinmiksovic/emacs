/* Native, headless remote recorder backend. */
#ifndef EMACS_REMOTETERM_H
#define EMACS_REMOTETERM_H
#include "remotegui.h"
#include "frame.h"
#include "dispextern.h"
#include "font.h"
struct remote_display_info {
  struct remote_display_info *next;
  struct terminal *terminal;
  struct remote_input *input;
  void *rdb;
  unsigned grabbed;
  struct frame *last_mouse_frame, *last_mouse_motion_frame;
  int last_mouse_motion_x, last_mouse_motion_y;
  int resx, resy, n_fonts, smallest_char_width, smallest_font_height;
  int n_planes;
  Lisp_Object name_list_element;
  Mouse_HLInfo mouse_highlight;
  struct frame *highlight_frame;
  Emacs_Cursor vertical_scroll_bar_cursor;
  unsigned long root_window;
  FILE *record;
  unsigned long events, glyphs;
  bool record_failed, record_ready, batch_open, in_update;
  unsigned long generation, transaction, presentations;
};
struct remote_output {
  struct remote_display_info *display_info;
  Emacs_Cursor text_cursor, hand_cursor, nontext_cursor;
  Emacs_Cursor horizontal_drag_cursor, vertical_drag_cursor, hourglass_cursor;
  Emacs_Cursor modeline_cursor, left_edge_cursor, top_left_corner_cursor;
  Emacs_Cursor top_edge_cursor, top_right_corner_cursor, right_edge_cursor;
  Emacs_Cursor bottom_right_corner_cursor, bottom_edge_cursor, bottom_left_corner_cursor;
  struct font *font;
  int fontset, baseline_offset;
  unsigned long parent_desc;
};
#define FRAME_OUTPUT_DATA(f) ((f)->output_data.remote)
#define FRAME_DISPLAY_INFO(f) (FRAME_OUTPUT_DATA(f)->display_info)
#define FRAME_FONT(f) (FRAME_OUTPUT_DATA(f)->font)
#define FRAME_FONTSET(f) (FRAME_OUTPUT_DATA(f)->fontset)
#define FRAME_BASELINE_OFFSET(f) (FRAME_OUTPUT_DATA(f)->baseline_offset)
#define FRAME_NATIVE_WINDOW(f) ((uintptr_t) (f)->terminal->id)
#define BLACK_PIX_DEFAULT(f) 0UL
#define WHITE_PIX_DEFAULT(f) 0xffffffUL
extern struct remote_display_info *x_display_list;
extern void remote_validate_frame_parameters (struct frame *, Lisp_Object);
extern void syms_of_remote (void);
#endif
