/* Host shim for lwip/sockets.h.
 *
 * The firmware needs exactly two things from it: SO_LINGER and struct linger,
 * used to turn an abandoned page send into a RST so its queued buffers come
 * back at once (see WEB_ABORT_ON_ABANDON in config.h). On the host there is no
 * socket to set an option on, so the values are the POSIX ones and the call
 * goes to the WiFiClient mock, which records it - which is what lets a test
 * assert that the abort path was taken without needing a TCP stack. */
#pragma once

#ifndef SOL_SOCKET
#define SOL_SOCKET 1
#endif
#ifndef SO_LINGER
#define SO_LINGER 13
#endif

struct linger {
  int l_onoff;
  int l_linger;
};

/* SO_SNDTIMEO bounds how long one write may block, which is what makes the
 * page send's own deadline enforceable - see WEB_SEND_SLICE_TIMEOUT_MS. */
#ifndef SO_SNDTIMEO
#define SO_SNDTIMEO 0x1005
#endif
