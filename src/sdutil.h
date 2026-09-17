/* ============================================================================
 *  sdutil.h - small card helpers shared by the modules that own files
 * ==========================================================================*/
#pragma once

#include <SD.h>

/* Remove a file that may not be there. SD.remove() on a missing file makes the
 * Arduino core print "remove(): ... does not exists" on the console, which
 * reads like a fault in a boot log that is otherwise clean. */
static inline void sdRemoveIfThere(const char *path) {
  if (SD.exists(path)) SD.remove(path);
}

/* After a write failed and the file was opened again: how much of the block
 * the card kept anyway, and how much from before the block it did not keep.
 *
 * write() reporting a failure does not mean nothing landed. FatFS writes whole
 * sectors as it goes, so a block can fail after its first sector is already in
 * the file. On the bench, rewriting the whole block put 29 bytes into the CSV
 * twice and broke a row. The file's length on the card is the truth:
 *   expect  bytes the file should hold before this block
 *   have    the length the card reports now
 *   block   the size of the block being written */
struct SdResume {
  size_t   done;         /* bytes of the block already in the file  */
  uint64_t lostBefore;   /* bytes before the block the card dropped */
};

static inline SdResume sdResume(uint64_t expect, uint64_t have, size_t block) {
  SdResume r = { 0, 0 };
  if (have < expect) {
    r.lostBefore = expect - have;
    return r;
  }
  const uint64_t extra = have - expect;
  r.done = (extra > (uint64_t)block) ? block : (size_t)extra;
  return r;
}
