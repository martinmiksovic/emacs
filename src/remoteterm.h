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
  int resx, resy, n_fonts, smallest_char_width, smallest_font_height;
  int n_planes;
  Lisp_Object name_list_element;
  Mouse_HLInfo mouse_highlight;
  struct frame *highlight_frame;
  Emacs_Cursor vertical_scroll_bar_cursor;
  unsigned long root_window;
  FILE *record;
  char *font_file;
  unsigned long events, glyphs;
  bool record_failed;
};
struct remote_output {
  struct remote_display_info *display_info;
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
#define FRAME_SMALLEST_CHAR_WIDTH(f) (FRAME_DISPLAY_INFO(f)->smallest_char_width)
#define FRAME_SMALLEST_FONT_HEIGHT(f) (FRAME_DISPLAY_INFO(f)->smallest_font_height)
#define BLACK_PIX_DEFAULT(f) 0UL
#define WHITE_PIX_DEFAULT(f) 0xffffffUL
extern struct remote_display_info *x_display_list;
extern void syms_of_remote (void);
#endif
