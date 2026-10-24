/* Single-peer, bounded live display stream.  GPL-3.0-or-later.
   Redisplay copies bytes; only process.c readiness callbacks send them.
   Socket data never contains Lisp, native structures or borrowed pointers. */
#include <config.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "lisp.h"
#include "remoteterm.h"
#include "remoterecord.h"
#include "remotestream.h"
#include "process.h"

/* A full supported frame must fit the queue, otherwise disconnect explicitly.
   Allocated once at listen time, never grown from a redisplay callback. */
enum { RECORD_LIMIT = 1024 * 1024, QUEUE_LIMIT = 8 * 1024 * 1024,
       IO_BUDGET = 64 * 1024, CONTROL_SIZE = 20 };
struct remote_stream
{
  struct frame *frame;             /* Owned by this frame's display context. */
  int listener, peer;
  bool active;
  unsigned used, line_used;
  unsigned char control[CONTROL_SIZE];
  char *queue, *line;
  size_t head, queued;
  unsigned long seq;
  char path[sizeof ((struct sockaddr_un *) 0)->sun_path];
  dev_t device;
  ino_t inode;
  EMACS_INT connections, rejected, overflow, disconnected;
};
static void stream_io (int, void *);
static void count (EMACS_INT *n) { if (*n < MOST_POSITIVE_FIXNUM) ++*n; }
static void
stream_disconnect (struct remote_stream *s)
{
  if (s->peer >= 0)
    {
      delete_write_fd (s->peer);
      delete_read_fd (s->peer);
      close (s->peer);
      s->peer = -1;
      count (&s->disconnected);
    }
  s->active = false;
  s->used = s->line_used = s->head = s->queued = 0;
}
static void
queue_bytes (struct remote_display_info *d, const char *bytes, size_t n)
{
  struct remote_stream *s = d->stream;
  if (n > QUEUE_LIMIT - s->queued)
    { count (&s->overflow); stream_disconnect (s); return; }
  size_t tail = (s->head + s->queued) % QUEUE_LIMIT;
  size_t first = min (n, QUEUE_LIMIT - tail);
  memcpy (s->queue + tail, bytes, first);
  memcpy (s->queue, bytes + first, n - first);
  s->queued += n;
  add_write_fd (s->peer, stream_io, d);
}
static void
queue_record (struct remote_display_info *d, const char *bytes, size_t n)
{
  struct remote_stream *s = d->stream;
  if (n > RECORD_LIMIT || n + 4 > QUEUE_LIMIT - s->queued)
    { count (&s->overflow); stream_disconnect (s); return; }
  char length[4] = { n >> 24, n >> 16, n >> 8, n };
  queue_bytes (d, length, sizeof length);
  queue_bytes (d, bytes, n);
}
void
remote_stream_start (struct remote_display_info *d, const char *event)
{
  struct remote_stream *s = d->stream;
  if (!s || !s->active) return;
  s->line_used = snprintf (s->line, RECORD_LIMIT,
                          "{\"seq\":%lu,\"event\":\"%s\"", s->seq++, event);
}
void
remote_stream_part (struct remote_display_info *d, const char *format, va_list ap)
{
  struct remote_stream *s = d->stream;
  if (!s || !s->active) return;
  unsigned left = RECORD_LIMIT - s->line_used;
  int n = vsnprintf (s->line + s->line_used, left, format, ap);
  if (n < 0 || n >= left)
    { count (&s->overflow); stream_disconnect (s); return; }
  s->line_used += n;
}
void
remote_stream_finish (struct remote_display_info *d)
{
  struct remote_stream *s = d->stream;
  if (s && s->active) queue_record (d, s->line, s->line_used);
}
static uint32_t
word (const unsigned char *p)
{ return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3]; }

/* Ordinary process readiness callback; never called by the SIGIO reader.
   Bounded control reads, one resize per callback, bounded nonblocking writes. */
static void
stream_io (int fd, void *data)
{
  struct remote_display_info *d = data;
  struct remote_stream *s = d->stream;
  bool hello = false, resize = false;
  unsigned width = 0, height = 0, scale = 0;
  for (int work = 0; work < 16; ++work)
    {
      unsigned target = s->used < 4 ? 4 : CONTROL_SIZE;
      ssize_t n = recv (fd, s->control + s->used, target - s->used, MSG_DONTWAIT);
      if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
      if (n <= 0) { stream_disconnect (s); return; }
      s->used += n;
      if (s->used >= 4 && word (s->control) != CONTROL_SIZE - 4) goto malformed;
      if (s->used < CONTROL_SIZE) continue;
      const unsigned char *p = s->control + 4;
      if (p[0] != 1 || p[2] || p[3]) goto malformed;
      if (p[1] == 1 && !s->active && !hello) hello = true;
      else if (p[1] != 2 || (!s->active && !hello)) goto malformed;
      width = word (p + 4); height = word (p + 8); scale = word (p + 12);
      if (width < 200 || width > 2048 || height < 114 || height > 2048
          || (scale != 1 && scale != 2)) goto malformed;
      resize = true;
      s->used = 0;
    }
  if (resize)
    {
      Lisp_Object frame;
      XSETFRAME (frame, s->frame);
      /* Finish the old transaction before changing the geometry or introducing
         the snapshot.  The wire's counters start at zero on every attachment. */
      remote_end (s->frame);
      d->scale = scale;
      Fset_frame_size (frame, make_fixnum (width), make_fixnum (height), Qt);
      remote_end (s->frame);
      if (hello)
        {
          char welcome[256];
          int n = snprintf (welcome, sizeof welcome,
            "{\"protocol\":\"remote-emacs-live\",\"version\":1,\"display_version\":1,"
            "\"max_record\":%d,\"transaction_base\":%lu,\"presentation_base\":%lu}",
            RECORD_LIMIT, d->transaction, d->presentations);
          queue_record (d, welcome, n);
          s->seq = 0; s->active = true;
          d->snapshot_only = true;
          remote_record_bootstrap (s->frame);
          d->snapshot_only = false;
        }
      Fredraw_frame (frame);
      /* Normal redisplay owns glyph generation and the presentation boundary. */
      Fredisplay (Qt);
    }
  if (s->peer < 0) return;
  if (s->queued)
    {
      size_t nbytes = min (s->queued, min (QUEUE_LIMIT - s->head, IO_BUDGET));
      ssize_t n = send (fd, s->queue + s->head, nbytes, MSG_DONTWAIT | MSG_NOSIGNAL);
      if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
      if (n <= 0) { stream_disconnect (s); return; }
      s->head = (s->head + n) % QUEUE_LIMIT;
      s->queued -= n;
    }
  if (!s->queued) delete_write_fd (fd);
  return;
 malformed:
  count (&s->rejected);
  stream_disconnect (s);
}
static void
stream_accept (int fd, void *data)
{
  struct remote_display_info *d = data;
  struct remote_stream *s = d->stream;
  int peer = accept4 (fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (peer < 0) return;
  struct ucred credentials;
  socklen_t size = sizeof credentials;
  if (peer >= FD_SETSIZE || s->peer >= 0
      || getsockopt (peer, SOL_SOCKET, SO_PEERCRED, &credentials, &size) < 0
      || credentials.uid != geteuid ())
    { close (peer); count (&s->rejected); return; }
  s->peer = peer;
  count (&s->connections);
  add_non_keyboard_read_fd (peer, stream_io, d);
}
void
remote_stream_close (struct remote_display_info *d)
{
  struct remote_stream *s = d->stream;
  if (!s) return;
  stream_disconnect (s);
  delete_read_fd (s->listener);
  close (s->listener);
  struct stat st;
  if (lstat (s->path, &st) == 0 && S_ISSOCK (st.st_mode)
      && st.st_dev == s->device && st.st_ino == s->inode) unlink (s->path);
  d->stream = NULL;
  xfree (s->line); xfree (s->queue); xfree (s);
}
DEFUN ("remote-display-listen", Fremote_display_listen, Sremote_display_listen, 2, 2, 0,
       doc: /* Listen for one display peer for FRAME on private Unix SOCKET.
This is a drawing/resize endpoint, never a Lisp evaluation endpoint.
A new connection requires a version 1 hello and receives a fresh full snapshot.
Deleting FRAME closes the endpoint and removes its owned socket pathname. */)
  (Lisp_Object frame, Lisp_Object path)
{
  CHECK_LIVE_FRAME (frame);
  struct frame *f = XFRAME (frame);
  if (!FRAME_REMOTE_P (f)) error ("Not a remote frame");
  struct remote_display_info *d = FRAME_DISPLAY_INFO (f);
  if (d->stream) error ("Remote display already listening");
  CHECK_STRING (path);
  path = Fexpand_file_name (path, Qnil);
  struct sockaddr_un addr = { .sun_family = AF_UNIX };
  if (SBYTES (path) >= sizeof addr.sun_path || memchr (SDATA (path), 0, SBYTES (path)))
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
  struct remote_stream *s = xzalloc (sizeof *s);
  s->frame = f; s->peer = -1;
  strcpy (s->path, addr.sun_path);
  int fd = socket (AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0 || fd >= FD_SETSIZE)
    {
      if (fd >= 0) close (fd);
      xfree (s); error ("Cannot allocate remote display socket");
    }
  bool bound = bind (fd, (struct sockaddr *) &addr, sizeof addr) == 0;
  if (!bound || chmod (addr.sun_path, 0600) < 0 || listen (fd, 1) < 0
      || lstat (addr.sun_path, &st) < 0)
    {
      int saved = errno;
      close (fd);
      if (bound) unlink (addr.sun_path);
      xfree (s); errno = saved;
      report_file_error ("Creating remote display socket", path);
    }
  s->listener = fd; s->device = st.st_dev; s->inode = st.st_ino;
  s->line = xmalloc (RECORD_LIMIT);
  s->queue = xmalloc (QUEUE_LIMIT);
  d->stream = s;
  add_non_keyboard_read_fd (fd, stream_accept, d);
  return path;
}
DEFUN ("remote-display-status", Fremote_display_status, Sremote_display_status, 1, 1, 0,
       doc: /* Return [LISTENING CONNECTED ACTIVE QUEUED CONNECTIONS REJECTED OVERFLOW DISCONNECTED].
QUEUED is bounded by eight MiB; overflow invalidates and disconnects the stream. */)
  (Lisp_Object frame)
{
  CHECK_LIVE_FRAME (frame);
  struct frame *f = XFRAME (frame);
  if (!FRAME_REMOTE_P (f)) error ("Not a remote frame");
  struct remote_stream *s = FRAME_DISPLAY_INFO (f)->stream;
  Lisp_Object result = Fmake_vector (make_fixnum (8), make_fixnum (0));
  ASET (result, 0, s ? Qt : Qnil);
  ASET (result, 1, s && s->peer >= 0 ? Qt : Qnil);
  ASET (result, 2, s && s->active ? Qt : Qnil);
  if (s)
    {
      ASET (result, 3, make_fixnum (s->queued));
      ASET (result, 4, make_fixnum (s->connections));
      ASET (result, 5, make_fixnum (s->rejected));
      ASET (result, 6, make_fixnum (s->overflow));
      ASET (result, 7, make_fixnum (s->disconnected));
    }
  return result;
}
void syms_of_remote_stream (void)
{ defsubr (&Sremote_display_listen); defsubr (&Sremote_display_status); }
