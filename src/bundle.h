/* ============================================================================
 *  bundle.h - one file on the card that carries the whole setup
 *
 *  A working setup is three files that must agree: /frames.dbc, /frames2.dbc
 *  and /dash.cfg. The layout refers to signals as "Message.Signal", so a map
 *  that has been pruned or had its names shortened, paired with a layout that
 *  has not, is broken in a way that looks like a firmware fault: every cell
 *  reads "unknown". Copying three files by hand in the right combination, every
 *  time a map is adjusted, is how that happens.
 *
 *  tools/make_bundle.py packs them into one:
 *
 *      #DCLB1 name_max=32
 *      #FILE frames.dbc 8106
 *      <8106 bytes, verbatim>
 *      #FILE frames2.dbc 48456
 *      <48456 bytes>
 *      #FILE dash.cfg 3011
 *      <3011 bytes>
 *      #END
 *
 *  Byte counts rather than delimiters, because a .dbc may contain any line a
 *  delimiter could be and a count cannot be spoofed by content. Line-oriented
 *  so this can unpack by streaming: no seeking, no parser, and one small stack
 *  buffer rather than anything from the heap - which matters here more than
 *  most places, since the heap it would take from is the one the dashboard
 *  needs.
 *
 *  THE BUNDLE IS THE SOURCE OF TRUTH. If one is present it is unpacked on every
 *  boot, overwriting the three files. That is deliberate: the alternative is a
 *  card whose .dbc and .cfg have drifted apart from the bundle that produced
 *  them, which is the exact failure this exists to remove. Delete the bundle to
 *  go back to editing the files directly.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include "config.h"

struct BundleInfo {
  bool     found;        /* a bundle was on the card                        */
  bool     ok;           /* it unpacked without error                       */
  uint8_t  files;        /* how many files it wrote                         */
  uint16_t nameMax;      /* the DBC_NAME_MAX the maps were prepared for     */
  /* 96, not 48: the messages carry a path, paths here run to 64 characters,
   * and the compiler was right to say so - a truncated reason is worse than
   * no reason, because it reads like a different fault. */
  char     err[96];      /* why it failed, empty when ok                    */
};

/* Unpack /logger.bundle, if it is there. Call after the card is mounted and
 * BEFORE the frame maps are read, since it is what puts them on the card.
 * Safe to call with no card and no bundle: it reports found=false and does
 * nothing. */
BundleInfo bundleUnpack();

/* The name_max the last unpacked bundle declared, or 0 if there was none.
 * Compared against DBC_NAME_MAX so a mismatch is said out loud rather than
 * showing up as silently clipped signal names. */
uint16_t bundleNameMax();
