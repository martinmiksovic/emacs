/* Native remote recorder resource types.  No display server resources. */
#ifndef EMACS_REMOTEGUI_H
#define EMACS_REMOTEGUI_H
typedef void *Emacs_Cursor;
typedef unsigned long Emacs_Pixmap;
typedef Emacs_Pixmap Emacs_Pix_Container;
typedef Emacs_Pixmap Emacs_Pix_Context;
#define NativeRectangle Emacs_Rectangle
#define STORE_NATIVE_RECT(r,a,b,c,d) ((r).x=(a),(r).y=(b),(r).width=(c),(r).height=(d))
#define CONVERT_TO_EMACS_RECT(a,b) ((a)=(b))
#define CONVERT_FROM_EMACS_RECT(a,b) ((a)=(b))
#endif
