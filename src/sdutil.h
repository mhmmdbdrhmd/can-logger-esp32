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
