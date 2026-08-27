/* ============================================================================
 *  dashstore.h - where the dashboard configuration is kept
 *
 *  Two copies, one rule. See dash.h for the reasoning; this is the mechanism.
 *
 *      NVS (the ESP32's own flash)   the live copy. Survives a card swap, a
 *                                    reformat, and running with no card at all.
 *      /dash.cfg (the SD card)       the same thing as text. Editable in any
 *                                    editor, copyable between loggers, and the
 *                                    only way to configure a logger without a
 *                                    browser.
 *
 *  THE RULE, in the order it runs at boot:
 *
 *    1. Load NVS. Whatever is there is the configuration.
 *    2. Look at /dash.cfg. NVS also remembers a hash of the text it last
 *       agreed with the card about.
 *         - hashes match      -> the card has not been touched. NVS wins,
 *                                which is what preserves anything saved from
 *                                the browser since the last boot.
 *         - hashes differ     -> somebody edited the file. The card wins, and
 *                                NVS is updated to match.
 *         - no file on card   -> the card is new. NVS is written out to it, so
 *                                a config can be copied to another logger.
 *         - no config anywhere -> an empty grid, which the browser fills in.
 *
 *  The effect is the one people expect - whichever copy you edited last is the
 *  one you get - without either silently overwriting the other every boot.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include "dash.h"

/* Opens NVS and loads the stored configuration into g_dash. Call before
 * recorderLoadDash(). Safe with no card and safe on a first boot. */
void dashStoreBegin();

/* ---------------------------------------------------------------------------
 *  WHY THE FLASH COPY IS WRITTEN LATE
 *
 *  NVS lives in the same SPI flash the program is executing from, so writing it
 *  disables the flash cache on both cores for the duration. An interrupt whose
 *  handler is not resident in IRAM cannot run while that is true.
 *
 *  canIsr() in app.cpp IS in IRAM - but attachInterrupt() reaches it through
 *  the Arduino core's shared GPIO dispatcher, and whether THAT is resident was
 *  decided when the core was built, not by anything in this project. So a save
 *  during a recording could cost the CAN interrupt a few milliseconds, and the
 *  MCP2515 holds two frames. On a busy bus that is lost data, and it would be
 *  lost because somebody dragged a gauge.
 *
 *  So a save marks the configuration dirty and writes /dash.cfg on the SD card
 *  - a plain SPI peripheral, no flash involved - and the NVS copy is written
 *  only once no recording is running.
 *
 *  Losing power in between costs nothing: the card holds the new layout, its
 *  hash no longer matches what NVS remembers, and the boot rule therefore
 *  imports it. The deferral is an optimisation on top of a design that was
 *  already correct without it.
 * -------------------------------------------------------------------------*/

/* Marks the configuration as needing to reach flash, and writes it immediately
 * if that is safe right now. Always returns quickly. */
bool dashStoreSave();

/* Writes the pending copy if it is now safe to. Call from loop(), and before
 * any deliberate restart. Cheap when there is nothing to do. */
void dashStoreService();

/* True while flash is behind the live configuration. */
bool dashStorePending();

/* The hash of the /dash.cfg text this logger last agreed with. 0 = never seen
 * one. Kept in RAM and written to flash with everything else. */
uint32_t dashStoreCardHash();
void     dashStoreNoteCardHash(uint32_t hash);

/* True when NVS held a configuration at boot - as opposed to this being a
 * logger that has never been configured. */
bool dashStoreHadConfig();
