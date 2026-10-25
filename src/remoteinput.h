/* Private native input transport.  GPL-3.0-or-later. */
#ifndef EMACS_REMOTEINPUT_H
#define EMACS_REMOTEINPUT_H
struct remote_input;
extern int remote_read_socket (struct terminal *, struct input_event *);
extern void remote_mouse_position (struct frame **, int, Lisp_Object *,
                                   enum scroll_bar_part *, Lisp_Object *,
                                   Lisp_Object *, Time *);
extern void remote_input_close (struct remote_display_info *);
extern void syms_of_remote_input (void);
#endif
