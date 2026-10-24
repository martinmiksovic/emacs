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

/* Four-byte big-endian length, then version, kind, modifiers, reserved,
   and a four-byte Unicode scalar or stable symbolic key ID. */
enum { REMOTE_PACKET_SIZE = 12, REMOTE_READ_BUDGET = 64 };
struct remote_input
{
  struct frame *frame;             /* Owned by the enclosing live frame. */
  int listener, peer;
  unsigned used;
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
  in->used = 0;
  d->highlight_frame = NULL;
  SET_FRAME_GARBAGED (in->frame);
}

/* This hook can run from SIGIO.  No allocation, Lisp evaluation or stdio.
   Keyboard queue ownership and C-g handling belong to keyboard.c. */
int
remote_read_socket (struct terminal *t, struct input_event *hold_quit)
{
  struct remote_display_info *d = t->display_info.remote;
  struct remote_input *in = d->input;
  if (!in || in->peer < 0 || kbd_on_hold_p ()) return 0;
  int events = 0;
  for (int work = 0; work < REMOTE_READ_BUDGET && !kbd_on_hold_p (); ++work)
    {
      unsigned target = in->used < 4 ? 4 : REMOTE_PACKET_SIZE;
      ssize_t n = recv (in->peer, in->packet + in->used, target - in->used,
                        MSG_DONTWAIT);
      if (n < 0 && (errno == EAGAIN || errno == EINTR))
        break;
      if (n <= 0)
        {
          if (in->used) remote_count (&in->rejected);
          remote_disconnect (d);
          break;
        }
      in->used += n;
      if (in->used >= 4 && (in->packet[0] || in->packet[1]
                           || in->packet[2] || in->packet[3] != 8))
        goto malformed;
      if (in->used < REMOTE_PACKET_SIZE) continue;
      const unsigned char *p = in->packet + 4;
      uint32_t code = ((uint32_t) p[4] << 24) | ((uint32_t) p[5] << 16)
        | ((uint32_t) p[6] << 8) | p[7];
      if (p[0] != 1 || p[3] || (p[2] & ~3)) goto malformed;
      struct input_event event;
      EVENT_INIT (event);
      event.modifiers = (p[2] & 1 ? ctrl_modifier : 0)
        | (p[2] & 2 ? meta_modifier : 0);
      XSETFRAME (event.frame_or_window, in->frame);
      if (p[1] == 1)
        {
          /* Text is a printable Unicode scalar.  Control keys are modifiers
             on printable characters; return/tab/DEL are symbolic keys. */
          if (code < 32 || (code >= 127 && code <= 159) || code > 0x10ffff
              || (code >= 0xd800 && code <= 0xdfff)) goto malformed;
          event.kind = code < 128 ? ASCII_KEYSTROKE_EVENT
            : MULTIBYTE_CHAR_KEYSTROKE_EVENT;
          event.code = code;
        }
      else if (p[1] == 2)
        {
          /* Values index keyboard.c's existing lispy_function_keys table.
             These are not exposed as platform keysyms on the wire. */
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
      in->used = 0;
      remote_count (&in->accepted);
      kbd_buffer_store_event_hold (&event, hold_quit);
      ++events;
      /* Let gobble_input deliver the quit immediately.  Do not drain an
         unbounded peer stream after a quit.  Later packets stay ordered. */
      if (hold_quit && hold_quit->kind != NO_EVENT) return 0;
      continue;
    malformed:
      remote_count (&in->rejected);
      remote_disconnect (d);
      break;
    }
  /* EOF is a peer disconnect, never the terminal-hangup return value -2. */
  return events;
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
          in->used = 0;
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
       doc: /* Listen for one native keyboard peer for FRAME at SOCKET.
SOCKET must not exist and must be inside a directory owned by this user
with no group/other permissions.  Deleting FRAME closes and removes it.
This Linux experiment accepts only version 1 bounded key packets. */)
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
