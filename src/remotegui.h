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
/* Generic frame geometry uses these X-compatible numeric flags on all ports. */
#define RGB_PIXEL_COLOR unsigned long
#define NorthWestGravity 1
#define NorthEastGravity 3
#define SouthWestGravity 7
#define SouthEastGravity 9
#define NoValue 0
#define XValue 1
#define YValue 2
#define WidthValue 4
#define HeightValue 8
#define XNegative 16
#define YNegative 32
#define USPosition 1
#define USSize 2
#define PPosition 4
#define PSize 8
#endif
