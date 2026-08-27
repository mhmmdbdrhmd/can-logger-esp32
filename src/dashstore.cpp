#include "dashstore.h"
#include "logger.h"
#include "recorder.h"

#include <Preferences.h>
#include <string.h>
#include <stdlib.h>

static Preferences s_nvs;
static bool        s_open      = false;
static bool        s_had       = false;
static uint32_t    s_cardHash  = 0;

/* Set when the live configuration differs from what is in flash. See the note
 * in the header for why this is not simply written on the spot. */
static volatile bool s_dirty     = false;
static volatile bool s_hashDirty = false;

/* One namespace, three keys. Deliberately short: NVS keys are limited to 15
 * characters and a truncated key is a silently different key. */
#define NVS_NS     "candash"
#define NVS_CFG    "cfg"
#define NVS_CARD   "cardhash"

bool dashStoreHadConfig() { return s_had; }
uint32_t dashStoreCardHash() { return s_cardHash; }

bool dashStorePending() { return s_dirty || s_hashDirty; }

void dashStoreNoteCardHash(uint32_t hash) {
  if (hash == s_cardHash) return;
  s_cardHash  = hash;
  s_hashDirty = true;
  dashStoreService();
}

void dashStoreBegin() {
  dashReset(g_dash);

  s_open = s_nvs.begin(NVS_NS, false);
  if (!s_open) {
    LOG_LIVE(LVL_WARN, "could not open the settings area in flash - the "
                       "dashboard layout will not be remembered");
    return;
  }

  s_cardHash = s_nvs.getUInt(NVS_CARD, 0);

  const size_t n = s_nvs.getBytesLength(NVS_CFG);
  if (n == 0 || n > DASH_CFG_MAX) {
    if (n > DASH_CFG_MAX) {
      LOG_FILE(LVL_WARN, "the stored dashboard layout is %u bytes, more than "
                         "the %u this build can hold - ignoring it",
               (unsigned)n, (unsigned)DASH_CFG_MAX);
    }
    return;
  }

  /* Read into the heap rather than the stack: this runs from setup(), whose
   * stack is not ours to spend 4 KB of. */
  char *buf = (char *)malloc(n + 1);
  if (!buf) {
    LOG_LIVE(LVL_WARN, "not enough memory to read the stored dashboard layout");
    return;
  }
  const size_t got = s_nvs.getBytes(NVS_CFG, buf, n);
  buf[got] = '\0';

  const uint16_t errors = dashParse(g_dash, buf, got);
  free(buf);

  s_had = true;
  if (errors) {
    LOG_FILE(LVL_WARN, "%u line(s) of the stored dashboard layout did not "
                       "parse - the rest was kept", (unsigned)errors);
  }
  LOG_FILE(LVL_INFO, "dashboard layout read from flash: %u bytes, %ux%u grid",
           (unsigned)got, (unsigned)g_dash.cols, (unsigned)g_dash.rows);
}

bool dashStoreSave() {
  s_dirty = true;
  dashStoreService();
  return s_open;
}

/* The actual flash write. Runs only when it cannot cost a frame. */
void dashStoreService() {
  if (!s_open) return;
  if (!s_dirty && !s_hashDirty) return;

  /* The one condition that matters. A recording is the only time the receive
   * path is carrying data that cannot be re-acquired. */
  if (g_rec.recording) return;

  if (s_hashDirty) {
    s_nvs.putUInt(NVS_CARD, s_cardHash);
    s_hashDirty = false;
  }
  if (!s_dirty) return;

  char *buf = (char *)malloc(DASH_CFG_MAX);
  if (!buf) return;                    /* stays dirty; tried again next pass */

  const size_t n = dashSerialize(g_dash, buf, DASH_CFG_MAX);
  if (n == 0 || n >= DASH_CFG_MAX) {
    /* Serialising filled the buffer, so the configuration is larger than this
     * build can store. Writing the truncated text would lose cells silently. */
    free(buf);
    s_dirty = false;
    LOG_LIVE(LVL_ERROR, "the dashboard layout is too large to store in flash "
                        "(over %u bytes) - the copy on the SD card is the one "
                        "that counts", (unsigned)DASH_CFG_MAX);
    return;
  }

  /* NVS is flash and the browser saves on every edit, so compare before
   * writing: an unchanged save costs an erase cycle for nothing. */
  const size_t have = s_nvs.getBytesLength(NVS_CFG);
  if (have == n) {
    char *old = (char *)malloc(have + 1);
    if (old) {
      const size_t got  = s_nvs.getBytes(NVS_CFG, old, have);
      const bool   same = (got == n) && (memcmp(old, buf, n) == 0);
      free(old);
      if (same) { free(buf); s_dirty = false; s_had = true; return; }
    }
  }

  const size_t wrote = s_nvs.putBytes(NVS_CFG, buf, n);
  free(buf);

  if (wrote != n) {
    LOG_LIVE(LVL_ERROR, "could not write the dashboard layout to flash - the "
                        "copy on the SD card is still correct");
    s_dirty = false;                   /* do not retry forever */
    return;
  }
  s_dirty = false;
  s_had   = true;
  LOG_FILE(LVL_INFO, "dashboard layout stored in flash (%u bytes)", (unsigned)n);
}
