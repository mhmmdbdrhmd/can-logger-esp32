/* ============================================================================
 *  CAN Logger ESP32 - entry point
 *
 *  Intentionally almost empty. When this file is copied to CanLogger.ino for
 *  the Arduino IDE the IDE preprocessor rewrites it - so nothing that could be
 *  damaged by that rewrite is allowed to live here. See app.h for the full
 *  explanation; the application itself is in app.cpp.
 * ==========================================================================*/

#include "app.h"

void setup() { appSetup(); }
void loop()  { appLoop();  }
