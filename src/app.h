/* ============================================================================
 *  app.h - the firmware entry points
 *
 *  The whole application lives in app.cpp rather than in the .ino, and this is
 *  not cosmetic. The Arduino IDE runs a preprocessor over .ino files that
 *  auto-generates a prototype for every function it finds. For an interrupt
 *  handler declared `static void IRAM_ATTR canIsr()`, the generated prototype
 *  carries no attribute, and GCC then drops the section attribute from the
 *  definition - silently relocating the CAN interrupt handler from IRAM into
 *  flash. It would run fine until the flash cache is disabled during an SPI
 *  flash operation, and then crash.
 *
 *  Keeping every real function in a .cpp sidesteps that preprocessor entirely,
 *  and makes the Arduino IDE and PlatformIO builds genuinely identical.
 * ==========================================================================*/
#pragma once

void appSetup();
void appLoop();
