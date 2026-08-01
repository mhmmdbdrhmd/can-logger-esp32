/* ============================================================================
 *  fmt.h - integer-only number formatting for the CSV hot path
 *
 *  A busy bus hands the logger several thousand numbers per second.
 *  printf-style conversion with %f drags in the soft-float formatter and costs
 *  tens of microseconds per call; worse, it rounds, so a value that left a node
 *  as the exact integer 12345 can come back as "12.344".
 *
 *  Almost every quantity on a CAN bus is a scaled integer to begin with - the
 *  DBC factor says so - and this file never converts one to floating point. It
 *  places a decimal point instead, so the CSV is bit-exact with respect to the
 *  wire. Only signals whose factor is genuinely not decimal go through
 *  fmtDouble, and the CSV header says when that happened.
 *
 *  All helpers append at *p and return the new end pointer; none write a
 *  terminating NUL (the caller adds the separator).
 * ==========================================================================*/
#pragma once

#include <Arduino.h>
#include <stdio.h>

/* Unsigned 64-bit -> decimal. */
inline char *fmtU64(char *p, uint64_t v) {
  char tmp[20];
  uint8_t n = 0;
  do { tmp[n++] = char('0' + (v % 10)); v /= 10; } while (v);
  while (n) *p++ = tmp[--n];
  return p;
}

/* Unsigned 32-bit -> decimal. */
inline char *fmtU32(char *p, uint32_t v) {
  return fmtU64(p, (uint64_t)v);
}

/* Signed 64-bit -> decimal. */
inline char *fmtI64(char *p, int64_t v) {
  uint64_t u;
  if (v < 0) { *p++ = '-'; u = (uint64_t)(-(v + 1)) + 1u; }  /* INT64_MIN safe */
  else       { u = (uint64_t)v; }
  return fmtU64(p, u);
}

/* Fixed point: `scaled` / 10^decimals, fractional part zero-padded.
 *   fmtFixed(p, 12345, 3) -> "12.345"      fmtFixed(p,  7, 3) -> "0.007"
 *   fmtFixed(p,    -3, 4) -> "-0.0003"     fmtFixed(p, 42, 0) -> "42"
 *
 * The sign is emitted before the magnitude is split, so a value between -1 and
 * 0 keeps its minus sign instead of collapsing through integer division. */
inline char *fmtFixed(char *p, int64_t scaled, uint8_t decimals) {
  uint64_t mag;
  if (scaled < 0) { *p++ = '-'; mag = (uint64_t)(-(scaled + 1)) + 1u; }
  else            { mag = (uint64_t)scaled; }

  if (!decimals) return fmtU64(p, mag);

  uint64_t d = 1;
  for (uint8_t i = 0; i < decimals; i++) d *= 10u;

  p = fmtU64(p, mag / d);
  *p++ = '.';
  uint64_t frac = mag % d;
  for (uint64_t scale = d / 10u; scale; scale /= 10u) {
    *p++ = char('0' + ((frac / scale) % 10u));
  }
  return p;
}

/* The escape hatch, for the rare signal whose factor is not a decimal literal.
 * %.10g keeps every digit a double can justify without printing noise. */
inline char *fmtDouble(char *p, double v) {
  const int n = snprintf(p, 32, "%.10g", v);
  return (n > 0) ? p + n : p;
}

/* Two uppercase hex nibbles. */
inline char *fmtHex8(char *p, uint8_t v) {
  static const char kDigits[] = "0123456789ABCDEF";
  *p++ = kDigits[v >> 4];
  *p++ = kDigits[v & 0x0Fu];
  return p;
}

/* "0x" followed by the identifier.
 *
 * An 11-bit id prints with its leading zeros suppressed, a 29-bit id always
 * prints all eight digits. That is not cosmetic: 0x100 standard and 0x100
 * extended are different frames on the wire, and the CSV has one column for
 * both, so the width has to carry the distinction. "0x100" is always standard,
 * "0x00000100" is always extended. */
inline char *fmtCanId(char *p, uint32_t id, bool ext) {
  /* Not named HEX: the Arduino core's Print.h defines HEX as the integer 16
   * for print(x, HEX), and that macro would rewrite any such declaration. */
  static const char kDigits[] = "0123456789ABCDEF";
  *p++ = '0'; *p++ = 'x';
  int8_t sh = ext ? 28 : 8;
  if (!ext) while (sh > 0 && ((id >> sh) & 0xFu) == 0) sh -= 4;
  for (; sh >= 0; sh -= 4) *p++ = kDigits[(id >> sh) & 0xFu];
  return p;
}

/* "0x" followed by `v`, at least `minDigits` wide. */
inline char *fmtHexValue(char *p, uint32_t v, uint8_t minDigits) {
  static const char kDigits[] = "0123456789ABCDEF";
  *p++ = '0'; *p++ = 'x';
  int8_t sh = 28;
  while (sh > 0 && ((v >> sh) & 0xFu) == 0 && (sh / 4) >= minDigits) sh -= 4;
  for (; sh >= 0; sh -= 4) *p++ = kDigits[(v >> sh) & 0xFu];
  return p;
}
