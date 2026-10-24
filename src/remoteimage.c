/* Deliberately image-free remote recorder, GPL-3.0-or-later.  */
#include <config.h>
#include "lisp.h"
#include "remoteterm.h"

/* Keep the ordinary frame cache ownership contract, but accept no images.
   These are rejection paths, not an image renderer.  */
struct image_cache *make_image_cache (void)
{ return xzalloc (sizeof (struct image_cache)); }
void free_image_cache (struct frame *f) { xfree (FRAME_IMAGE_CACHE (f)); }
void mark_image_cache (struct image_cache *c) { eassert (c->used == 0); }
void clear_image_cache (struct frame *f, Lisp_Object filter) {}
void clear_image_caches (Lisp_Object filter) {}
void image_prune_animation_caches (bool all) {}
bool valid_image_p (Lisp_Object spec) { return false; }
ptrdiff_t lookup_image (struct frame *f, Lisp_Object spec, int face)
{ emacs_abort (); }
void prepare_image_for_display (struct frame *f, struct image *image)
{ emacs_abort (); }
int image_ascent (struct image *i, struct face *f, struct glyph_slice *s)
{ emacs_abort (); }
Lisp_Object image_spec_value (Lisp_Object spec, Lisp_Object key, bool *found)
{ if (found) *found = false; return Qnil; }
ptrdiff_t image_create_bitmap_from_data (struct frame *f, char *data,
                                        unsigned width, unsigned height)
{ return -1; }
ptrdiff_t image_create_bitmap_from_file (struct frame *f, Lisp_Object file)
{ return -1; }
void image_destroy_bitmap (struct frame *f, ptrdiff_t id)
{ eassert (id <= 0); }
void image_destroy_all_bitmaps (Display_Info *dpy) {}
void image_reference_bitmap (struct frame *f, ptrdiff_t id) { emacs_abort (); }
ptrdiff_t image_bitmap_pixmap (struct frame *f, ptrdiff_t id) { emacs_abort (); }
DEFUN ("image-transforms-p", Fimage_transforms_p, Simage_transforms_p, 0, 1, 0,
       doc: /* Return nil: this recorder does not support images.  */)
  (Lisp_Object frame) { return Qnil; }
DEFUN ("clear-image-cache", Fclear_image_cache, Sclear_image_cache, 0, 2, 0,
       doc: /* There are no images in the remote recorder.  */)
  (Lisp_Object filter, Lisp_Object frame) { return Qnil; }
void syms_of_image (void)
{
  DEFVAR_LISP ("image-types", Vimage_types,
              doc: /* No image types are supported by the remote recorder.  */);
  Vimage_types = Qnil;
  DEFSYM (Qauto, "auto");
  DEFSYM (QCascent, ":ascent");
  DEFSYM (QCmargin, ":margin");
  DEFSYM (QCrelief, ":relief");
  DEFSYM (QCconversion, ":conversion");
  defsubr (&Simage_transforms_p);
  defsubr (&Sclear_image_cache);
}
