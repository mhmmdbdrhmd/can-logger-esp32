/* ============================================================================
 *  webui.h - the operator-facing dashboard
 *
 *  Deliberately built on the core's plain WebServer with 2 Hz polling rather
 *  than an async server with websockets:
 *    - no third-party libraries, so the PlatformIO and Arduino IDE builds are
 *      the same code with no install steps,
 *    - the HTTP handler runs at the lowest priority on the application core and
 *      never touches the SD card or the CAN controller, so a browser hitting
 *      refresh cannot perturb a recording.
 *
 *  The page shows nothing bus-specific, because the firmware knows nothing
 *  bus-specific: card, recording, link health, integrity, and a live table of
 *  the identifiers actually on the wire.
 * ==========================================================================*/
#pragma once

#include <Arduino.h>

void webBegin();     /* starts the server on the configured port */
void webService();   /* call from loop() */
