/* Private bounded live display transport.  GPL-3.0-or-later. */
#ifndef EMACS_REMOTESTREAM_H
#define EMACS_REMOTESTREAM_H
#include <stdarg.h>
extern void remote_stream_start (struct remote_display_info *, const char *);
extern void remote_stream_part (struct remote_display_info *, const char *, va_list)
  ATTRIBUTE_FORMAT_PRINTF (2, 0);
extern void remote_stream_finish (struct remote_display_info *);
extern void remote_stream_close (struct remote_display_info *);
extern void syms_of_remote_stream (void);
#endif
