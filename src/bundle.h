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
 *  A bundle is unpacked ONCE. /logger.bundle is written over the three files
 *  and then renamed to /logger.applied, so a layout changed in the browser
 *  afterwards survives the next restart instead of being put back every boot.
 *  The applied copy is kept for its first line: the name length the maps were
 *  prepared for is needed on every boot, since it sizes the tables. Putting a
 *  new /logger.bundle on the card applies that one, the same way.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include "config.h"

struct BundleInfo {
  bool     found;        /* a NEW bundle was on the card                    */
  bool     applied;      /* no new one, but an earlier one set name_max     */
  bool     ok;           /* it unpacked without error                       */
  uint8_t  files;        /* how many files it wrote                         */
  uint16_t nameMax;      /* the DBC_NAME_MAX the maps were prepared for     */
  /* 96, not 48: the messages carry a path, paths here run to 64 characters,
   * and the compiler was right to say so - a truncated reason is worse than
   * no reason, because it reads like a different fault. */
  char     err[96];      /* why it failed, empty when ok                    */
};

/* Unpack /logger.bundle, if it is there, and set it aside as applied. With
 * no new bundle, reads the name length of the one applied last. Call after the
 * card is mounted and BEFORE the frame maps are read. Safe with no card and no
 * bundle: found=false, applied=false, nameMax=0, nothing done. */
BundleInfo bundleUnpack();

/* The name_max the bundle in force declared, or 0 if there is none. */
uint16_t bundleNameMax();
