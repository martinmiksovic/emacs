/* Private single-peer native keyboard input.  GPL-3.0-or-later.
   The wire contains keys, never Lisp or editor commands. */
#include <config.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "lisp.h"
#include "remoteterm.h"
#include "termhooks.h"
#include "keyboard.h"
#include "process.h"
#include "termopts.h"
#include "blockinput.h"
#include "remoteinput.h"

/* Four-byte big-endian body length.  V1 keys have an eight-byte body;
   v2 pointer events have a 24-byte body.  No native structures on the wire. */
enum { REMOTE_PACKET_SIZE = 28, REMOTE_READ_BUDGET = 64 };
struct remote_input
{
  struct frame *frame;             /* Owned by the enclosing live frame. */
  int listener, peer;
  unsigned used, buttons;
  bool yield, position_valid;
  int x, y;
  Time time;
  unsigned char packet[REMOTE_PACKET_SIZE];
  char path[sizeof ((struct sockaddr_un *) 0)->sun_path];
  dev_t device;
  ino_t inode;
  EMACS_INT accepted, rejected, disconnected;
};

static void
remote_count (EMACS_INT *value)
{
  if (*value < MOST_POSITIVE_FIXNUM) ++*value;
}

static void
remote_disconnect (struct remote_display_info *d)
{
  struct remote_input *in = d->input;
  if (in->peer >= 0)
    {
      delete_keyboard_wait_descriptor (in->peer);
      close (in->peer);
      in->peer = -1;
      remote_count (&in->disconnected);
    }
  in->used = in->buttons = 0;
  in->yield = in->position_valid = false;
  d->highlight_frame = NULL;
  SET_FRAME_GARBAGED (in->frame);
}

static uint32_t
remote_u32 (const unsigned char *p)
{
  return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16)
    | ((uint32_t) p[2] << 8) | p[3];
}

/* Decode two's complement without an implementation-defined unsigned-to-signed
   conversion.  Floor fixed-point coordinates, including negative drag positions. */
static int64_t
remote_coordinate (const unsigned char *p)
{
  uint32_t u = remote_u32 (p);
  return u <= INT32_MAX ? u : (int64_t) u - (INT64_C (1) << 32);
}

static bool
remote_pointer (struct remote_input *in, const unsigned char *p,
                struct input_event *event, unsigned *count)
{
  int64_t x = remote_coordinate (p + 12), y = remote_coordinate (p + 16);
  unsigned amount = ((unsigned) p[6] << 8) | p[7];
  unsigned button = p[4];
  if (p[0] != 2 || (p[2] & ~7) || (p[3] & ~7) || p[5]
      || remote_u32 (p + 20) || x < -33554432 || x > 33554432
      || y < -33554432 || y > 33554432)
    return false;
  event->modifiers = (p[2] & 1 ? ctrl_modifier : 0)
    | (p[2] & 2 ? meta_modifier : 0) | (p[2] & 4 ? shift_modifier : 0);
  switch (p[1])
    {
    case 3:                         /* Motion, including held-button motion. */
      if (button || amount || p[3] != in->buttons) return false;
      event->kind = MOUSE_MOTION_EVENT;
      event->code = p[3];
      break;
    case 4:                         /* Down; mask describes the new state. */
    case 5:                         /* Up. */
      if (button < 1 || button > 3 || amount) return false;
      unsigned bit = 1u << (button - 1);
      if ((p[1] == 4 && (in->buttons & bit))
          || (p[1] == 5 && !(in->buttons & bit))
          || p[3] != (in->buttons ^ bit)) return false;
      event->kind = MOUSE_CLICK_EVENT;
      event->code = button - 1;
      event->modifiers |= p[1] == 4 ? down_modifier : up_modifier;
      break;
    case 6:                         /* Discrete wheel detents. */
      if (button < 1 || button > 4 || !amount || amount > 8
          || p[3] != in->buttons) return false;
      event->kind = button <= 2 ? WHEEL_EVENT : HORIZ_WHEEL_EVENT;
      event->modifiers |= button & 1 ? up_modifier : down_modifier;
      *count = amount;
      break;
    default:
      return false;
    }
  in->buttons = p[3];
  in->x = (x - (x < 0 ? 1023 : 0)) / 1024;
  in->y = (y - (y < 0 ? 1023 : 0)) / 1024;
  in->time = remote_u32 (p + 8);
  in->position_valid = true;
  event->x = make_fixnum (in->x);
  event->y = make_fixnum (in->y);
  event->timestamp = in->time;
  return true;
}

/* This hook can run from SIGIO.  No allocation, Lisp evaluation or stdio.
   Keyboard queue ownership and C-g handling belong to keyboard.c. */
int
remote_read_socket (struct terminal *t, struct input_event *hold_quit)
{
  struct remote_display_info *d = t->display_info.remote;
  struct remote_input *in = d->input;
  if (!in || in->peer < 0 || kbd_on_hold_p ()) return 0;
  /* gobble_input repeatedly drains a positive-returning terminal before moving
     to the next.  Yield once after each bounded batch, even under a flood. */
  if (in->yield) { in->yield = false; return 0; }
  int events = 0;
  for (int work = 0; work < REMOTE_READ_BUDGET && !kbd_on_hold_p (); ++work)
    {
      unsigned target = in->used < 4 ? 4 : 4 + remote_u32 (in->packet);
      ssize_t n = recv (in->peer, in->packet + in->used, target - in->used,
                        MSG_DONTWAIT);
      if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
      if (n <= 0)
        {
          if (in->used) remote_count (&in->rejected);
          remote_disconnect (d);
          break;
        }
      in->used += n;
      unsigned length = in->used >= 4 ? remote_u32 (in->packet) : 0;
      if (in->used >= 4 && length != 8 && length != 24) goto malformed;
      if (in->used < length + 4) continue;
      const unsigned char *p = in->packet + 4;
      struct input_event event;
      EVENT_INIT (event);
      XSETFRAME (event.frame_or_window, in->frame);
      unsigned count = 1;
      if (length == 24)
        {
          if (!remote_pointer (in, p, &event, &count)) goto malformed;
        }
      else
        {
          uint32_t code = remote_u32 (p + 4);
          if (p[0] != 1 || p[3] || (p[2] & ~3)) goto malformed;
          event.modifiers = (p[2] & 1 ? ctrl_modifier : 0)
            | (p[2] & 2 ? meta_modifier : 0);
          if (p[1] == 1)
            {
              if (code < 32 || (code >= 127 && code <= 159) || code > 0x10ffff
                  || (code >= 0xd800 && code <= 0xdfff)) goto malformed;
              event.kind = code < 128 ? ASCII_KEYSTROKE_EVENT
                : MULTIBYTE_CHAR_KEYSTROKE_EVENT;
              event.code = code;
            }
          else if (p[1] == 2)
            {
              static const unsigned keys[] = {
                0, 0xff0d, 0xff09, 0xff08, 0xffff, 0xff1b,
                0xff51, 0xff53, 0xff52, 0xff54, 0xff50, 0xff57,
                0xff55, 0xff56, 0xff63,
                0xffbe, 0xffbf, 0xffc0, 0xffc1, 0xffc2, 0xffc3,
                0xffc4, 0xffc5, 0xffc6, 0xffc7, 0xffc8, 0xffc9
              };
              if (!code || code >= ARRAYELTS (keys)) goto malformed;
              event.kind = NON_ASCII_KEYSTROKE_EVENT;
              event.code = keys[code];
            }
          else goto malformed;
        }
      in->used = 0;
      remote_count (&in->accepted);
      /* At most eight wheel events can cross the half-full ring threshold;
         this cannot fill the remaining half of Emacs's keyboard ring. */
      for (unsigned i = 0; i < count; ++i)
        kbd_buffer_store_event_hold (&event, hold_quit);
      events += count;
      if (hold_quit && hold_quit->kind != NO_EVENT) return 0;
      continue;
    malformed:
      remote_count (&in->rejected);
      remote_disconnect (d);
      break;
    }
  in->yield = events > 0;
  /* EOF is a peer disconnect, never the terminal-hangup return value -2. */
  return events;
}

void
remote_mouse_position (struct frame **frame, int insist,
                       Lisp_Object *bar_window, enum scroll_bar_part *part,
                       Lisp_Object *x, Lisp_Object *y, Time *time)
{
  block_input ();
  struct remote_input *in = FRAME_DISPLAY_INFO (*frame)->input;
  *bar_window = Qnil;
  *part = 0;
  (*frame)->mouse_moved = false;
  if (in && in->position_valid)
    {
      *x = make_fixnum (in->x);
      *y = make_fixnum (in->y);
      *time = in->time;
    }
  else
    *frame = NULL;
  unblock_input ();
}

/* Listener readiness runs in the ordinary process wait loop, not SIGIO. */
static void
remote_accept (int fd, void *data)
{
  struct remote_display_info *d = data;
  block_input ();
  struct remote_input *in = d->input;
  int peer = accept4 (fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (peer >= 0)
    {
      struct ucred credentials;
      socklen_t size = sizeof credentials;
      if (peer >= FD_SETSIZE || in->peer >= 0
          || getsockopt (peer, SOL_SOCKET, SO_PEERCRED, &credentials, &size) < 0
          || credentials.uid != geteuid ()
          || fcntl (peer, F_SETOWN, getpid ()) < 0)
        {
          close (peer);
          remote_count (&in->rejected);
        }
      else
        {
          in->peer = peer;
          in->used = in->buttons = 0;
          in->yield = in->position_valid = false;
          struct input_event reset;
          EVENT_INIT (reset);
          reset.kind = MOUSE_RESET_EVENT;
          XSETFRAME (reset.frame_or_window, in->frame);
          kbd_buffer_store_event (&reset);
          d->highlight_frame = in->frame;
          SET_FRAME_GARBAGED (in->frame);
          add_keyboard_wait_descriptor (peer);
          if (interrupt_input) init_sigio (peer);
          /* Read bytes sent before FASYNC was enabled as well, after
             leaving this critical section.  Delivering a quit here could
             longjmp past unblock_input and leave input blocked forever. */
          pending_signals = true;
        }
    }
  unblock_input ();
}

void
remote_input_close (struct remote_display_info *d)
{
  if (!d->input) return;
  block_input ();
  struct remote_input *in = d->input;
  remote_disconnect (d);
  delete_read_fd (in->listener);
  close (in->listener);
  /* Do not delete a replacement path supplied by another process. */
  struct stat st;
  if (lstat (in->path, &st) == 0 && S_ISSOCK (st.st_mode)
      && st.st_dev == in->device && st.st_ino == in->inode)
    unlink (in->path);
  d->input = NULL;
  xfree (in);
  unblock_input ();
}

DEFUN ("remote-input-listen", Fremote_input_listen, Sremote_input_listen, 2, 2, 0,
       doc: /* Listen for one native keyboard/pointer peer for FRAME at SOCKET.
SOCKET must not exist and must be inside a directory owned by this user
with no group/other permissions.  Deleting FRAME closes and removes it.
Accept version 1 bounded key packets and version 2 bounded pointer packets. */)
  (Lisp_Object frame, Lisp_Object path)
{
  CHECK_LIVE_FRAME (frame);
  struct frame *f = XFRAME (frame);
  if (!FRAME_REMOTE_P (f)) error ("Not a remote frame");
  struct remote_display_info *d = FRAME_DISPLAY_INFO (f);
  if (d->input) error ("Remote input already listening");
  CHECK_STRING (path);
  path = Fexpand_file_name (path, Qnil);
  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  if (SBYTES (path) >= sizeof addr.sun_path
      || memchr (SDATA (path), 0, SBYTES (path)))
    error ("Invalid remote socket pathname");
  memcpy (addr.sun_path, SDATA (path), SBYTES (path));
  char parent[sizeof addr.sun_path];
  strcpy (parent, addr.sun_path);
  char *slash = strrchr (parent, '/');
  if (!slash || slash == parent) error ("Remote socket needs a private directory");
  *slash = 0;
  struct stat st;
  if (lstat (parent, &st) < 0 || !S_ISDIR (st.st_mode)
      || st.st_uid != geteuid () || (st.st_mode & 0077))
    error ("Remote socket directory must be private and owned by this user");
  struct remote_input *in = xzalloc (sizeof *in);
  in->frame = f; in->peer = -1;
  strcpy (in->path, addr.sun_path);
  int fd = socket (AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0 || fd >= FD_SETSIZE)
    {
      if (fd >= 0) close (fd);
      xfree (in);
      error ("Cannot allocate remote input socket");
    }
  bool bound = bind (fd, (struct sockaddr *) &addr, sizeof addr) == 0;
  if (!bound || chmod (addr.sun_path, 0600) < 0 || listen (fd, 1) < 0
      || lstat (addr.sun_path, &st) < 0)
    {
      int saved_errno = errno;
      close (fd);
      if (bound) unlink (addr.sun_path);
      xfree (in);
      errno = saved_errno;
      report_file_error ("Creating remote input socket", path);
    }
  in->listener = fd; in->device = st.st_dev; in->inode = st.st_ino;
  block_input ();
  d->input = in;
  add_non_keyboard_read_fd (fd, remote_accept, d);
  unblock_input ();
  return path;
}

DEFUN ("remote-input-status", Fremote_input_status, Sremote_input_status, 1, 1, 0,
       doc: /* Return [LISTENING CONNECTED ACCEPTED REJECTED DISCONNECTED PARTIAL]
for FRAME's native input transport.  Counters count parsed packets, not commands. */)
  (Lisp_Object frame)
{
  CHECK_LIVE_FRAME (frame);
  struct frame *f = XFRAME (frame);
  if (!FRAME_REMOTE_P (f)) error ("Not a remote frame");
  Lisp_Object result = Fmake_vector (make_fixnum (6), make_fixnum (0));
  block_input ();
  struct remote_input *in = FRAME_DISPLAY_INFO (f)->input;
  ASET (result, 0, in ? Qt : Qnil);
  ASET (result, 1, in && in->peer >= 0 ? Qt : Qnil);
  if (in)
    {
      ASET (result, 2, make_fixnum (in->accepted));
      ASET (result, 3, make_fixnum (in->rejected));
      ASET (result, 4, make_fixnum (in->disconnected));
      ASET (result, 5, make_fixnum (in->used));
    }
  unblock_input ();
  return result;
}

void
syms_of_remote_input (void)
{
  defsubr (&Sremote_input_listen);
  defsubr (&Sremote_input_status);
}
