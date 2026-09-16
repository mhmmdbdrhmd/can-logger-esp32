#include "bundle.h"
#include "logger.h"
#include <SD.h>

/* The one buffer this uses. 512 bytes is an SD sector and comfortably more than
 * the longest header line; making it larger would only move bytes in bigger
 * gulps, and the heap it would come from is the heap the dashboard needs. */
static const size_t COPY_BUF = 512;

static uint16_t s_nameMax = 0;

uint16_t bundleNameMax() { return s_nameMax; }

/* Read one line, without allocating and without Arduino's String.
 * Returns the length, or -1 at end of file. Trailing \r is dropped so a bundle
 * written on Windows unpacks the same as one written anywhere else - which is
 * the likely case, since these come off a laptop. */
static int readLine(File &f, char *buf, size_t cap) {
  size_t n = 0;
  if (!f.available()) return -1;
  while (f.available() && n < cap - 1) {
    const int c = f.read();
    if (c < 0) break;
    if (c == '\n') break;
    buf[n++] = (char)c;
  }
  while (n && (buf[n - 1] == '\r')) n--;
  buf[n] = '\0';
  return (int)n;
}

BundleInfo bundleUnpack() {
  BundleInfo bi;
  bi.found = false;
  bi.ok = false;
  bi.files = 0;
  bi.nameMax = 0;
  bi.err[0] = '\0';
  s_nameMax = 0;

  if (!SD.exists(BUNDLE_PATH)) return bi;
  bi.found = true;

  File f = SD.open(BUNDLE_PATH, FILE_READ);
  if (!f) {
    snprintf(bi.err, sizeof(bi.err), "cannot open %s", BUNDLE_PATH);
    return bi;
  }

  char line[160];
  if (readLine(f, line, sizeof(line)) < 0 || strncmp(line, "#DCLB1", 6) != 0) {
    /* Refuse rather than guess. A file that is not a bundle must not be
     * half-unpacked over a working set of maps. */
    snprintf(bi.err, sizeof(bi.err), "not a bundle (no #DCLB1 header)");
    f.close();
    return bi;
  }

  {
    const char *p = strstr(line, "name_max=");
    if (p) {
      bi.nameMax = (uint16_t)atoi(p + 9);
      s_nameMax = bi.nameMax;
    }
  }

  uint8_t buf[COPY_BUF];
  bool done = false;

  while (!done) {
    const int n = readLine(f, line, sizeof(line));
    if (n < 0) {
      /* Ran out before #END: the file was truncated in transit. Say so - the
       * files already written are still valid, and which ones is worth
       * knowing. */
      snprintf(bi.err, sizeof(bi.err), "truncated after %u file(s)",
               (unsigned)bi.files);
      break;
    }
    if (strncmp(line, "#END", 4) == 0) { done = true; break; }
    if (strncmp(line, "#FILE ", 6) != 0) continue;   /* comments, blank lines */

    char name[48];
    unsigned long want = 0;
    if (sscanf(line + 6, "%47s %lu", name, &want) != 2) {
      snprintf(bi.err, sizeof(bi.err), "bad #FILE line");
      break;
    }

    char path[64];
    snprintf(path, sizeof(path), "/%s", name);

    /* Write to a temporary and rename only once the whole payload is there, so
     * a bundle that is cut short cannot leave a half-written frame map in
     * place of the good one that was there before. */
    char tmp[72];
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    SD.remove(tmp);
    File out = SD.open(tmp, FILE_WRITE);
    if (!out) {
      snprintf(bi.err, sizeof(bi.err), "cannot write %s", tmp);
      break;
    }

    unsigned long left = want;
    bool wrote = true;
    while (left) {
      const size_t chunk = (left < COPY_BUF) ? (size_t)left : COPY_BUF;
      const int got = f.read(buf, chunk);
      if (got <= 0) { wrote = false; break; }
      if (out.write(buf, (size_t)got) != (size_t)got) { wrote = false; break; }
      left -= (unsigned long)got;
    }
    out.close();

    if (!wrote) {
      SD.remove(tmp);
      snprintf(bi.err, sizeof(bi.err), "short read on %s", name);
      break;
    }
    SD.remove(path);
    if (!SD.rename(tmp, path)) {
      SD.remove(tmp);
      snprintf(bi.err, sizeof(bi.err), "cannot replace %s", path);
      break;
    }
    bi.files++;
    LOG_FILE(LVL_INFO, "bundle: wrote %s (%lu bytes)", path, want);

    /* The payload is followed by the newline make_bundle.py adds so the next
     * header starts on its own line. Consume it if it is there. */
    if (f.available()) {
      const int c = f.peek();
      if (c == '\n' || c == '\r') f.read();
    }
  }

  f.close();
  bi.ok = (bi.err[0] == '\0') && bi.files > 0;
  return bi;
}
