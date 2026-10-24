#include <config.h>
#include "lisp.h"
#include "remoteterm.h"
struct remote_display_info *x_display_list;
void syms_of_remote (void) { Fprovide (intern_c_string ("remote-display"), Qnil); }
