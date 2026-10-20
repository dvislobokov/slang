/* poll_kqueue.c - readiness poller for macOS and the BSDs (platform.h).
 * Not implemented yet: docs/PLATFORMS.md stage 2 ports the poller to
 * kqueue (EVFILT_READ/EVFILT_WRITE with EV_CLEAR off, EVFILT_USER for
 * wake-ups). Until then a build on these systems stops here. */
#include "platform.h"

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#error "platform/poll_kqueue.c: the kqueue I/O backend is not implemented yet (docs/PLATFORMS.md, stage 2)"
#else
typedef int slang__poll_kqueue_unused;
#endif
