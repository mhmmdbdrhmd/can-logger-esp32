#include "mcp2515.h"

/* ---- SPI instructions (datasheet table 12-1) ---------------------------- */
#define CMD_RESET        0xC0
#define CMD_READ         0x03
#define CMD_WRITE        0x02
#define CMD_BIT_MODIFY   0x05
#define CMD_READ_STATUS  0xA0
#define CMD_READ_RX0     0x90   /* starts at RXB0SIDH, auto-clears RX0IF     */
#define CMD_READ_RX1     0x94   /* starts at RXB1SIDH, auto-clears RX1IF     */
#define CMD_LOAD_TX0     0x40   /* starts at TXB0SIDH                        */
#define CMD_RTS_TX0      0x81   /* request-to-send, buffer 0                 */

/* ---- registers ---------------------------------------------------------- */
#define REG_CANSTAT      0x0E
#define REG_CANCTRL      0x0F
#define REG_TEC          0x1C
#define REG_REC          0x1D
#define REG_CNF3         0x28
#define REG_CNF2         0x29
#define REG_CNF1         0x2A
#define REG_CANINTE      0x2B
#define REG_CANINTF      0x2C
#define REG_EFLG         0x2D
#define REG_TXB0CTRL     0x30
#define REG_RXB0CTRL     0x60
#define REG_RXB1CTRL     0x70

/* CANINTE / CANINTF bits */
#define INT_RX0          0x01
#define INT_RX1          0x02
#define INT_ERR          0x20
#define INT_MERR         0x80

/* EFLG bits */
#define EFLG_RX0OVR      0x40
#define EFLG_RX1OVR      0x80

/* TXB0CTRL bits */
#define TXB_ABTF         0x40   /* the transmission was aborted             */
#define TXB_MLOA         0x20   /* arbitration was lost                     */
#define TXB_TXERR        0x10   /* a bus error occurred during transmission */
#define TXB_TXREQ        0x08   /* set to send, cleared by hardware or us   */

/* CANCTRL: one-shot mode - attempt a frame once, never retry forever */
#define CANCTRL_OSM      0x08

/* CANCTRL modes (top three bits) */
#define MODE_NORMAL      0x00
#define MODE_LISTENONLY  0x60
#define MODE_CONFIG      0x80
#define MODE_MASK        0xE0

MCP2515::MCP2515(SPIClass &spi, int8_t csPin, uint32_t spiHz)
    : _spi(spi), _cs(csPin), _cfg(spiHz, MSBFIRST, SPI_MODE0) {}

/* -------------------------------------------------------------------------
 *  Low-level register access. Each call is one self-contained SPI
 *  transaction, so the bus can be shared (it is not, here) and so a
 *  higher-priority task preempting us cannot leave CS asserted.
 * ---------------------------------------------------------------------- */
void MCP2515::reset() {
  _spi.beginTransaction(_cfg);
  select();
  _spi.transfer(CMD_RESET);
  deselect();
  _spi.endTransaction();
  delay(10);                       /* datasheet: oscillator start-up time */
}

uint8_t MCP2515::readReg(uint8_t addr) {
  _spi.beginTransaction(_cfg);
  select();
  _spi.transfer(CMD_READ);
  _spi.transfer(addr);
  uint8_t v = _spi.transfer(0x00);
  deselect();
  _spi.endTransaction();
  return v;
}

void MCP2515::readRegs(uint8_t addr, uint8_t *buf, uint8_t n) {
  _spi.beginTransaction(_cfg);
  select();
  _spi.transfer(CMD_READ);
  _spi.transfer(addr);
  for (uint8_t i = 0; i < n; i++) buf[i] = _spi.transfer(0x00);
  deselect();
  _spi.endTransaction();
}

void MCP2515::writeReg(uint8_t addr, uint8_t val) {
  _spi.beginTransaction(_cfg);
  select();
  _spi.transfer(CMD_WRITE);
  _spi.transfer(addr);
  _spi.transfer(val);
  deselect();
  _spi.endTransaction();
}

void MCP2515::modifyReg(uint8_t addr, uint8_t mask, uint8_t val) {
  _spi.beginTransaction(_cfg);
  select();
  _spi.transfer(CMD_BIT_MODIFY);
  _spi.transfer(addr);
  _spi.transfer(mask);
  _spi.transfer(val);
  deselect();
  _spi.endTransaction();
}

uint8_t MCP2515::readStatus() {
  _spi.beginTransaction(_cfg);
  select();
  _spi.transfer(CMD_READ_STATUS);
  uint8_t v = _spi.transfer(0x00);
  deselect();
  _spi.endTransaction();
  return v;
}

bool MCP2515::setMode(uint8_t mode) {
  modifyReg(REG_CANCTRL, MODE_MASK, mode);
  for (uint8_t i = 0; i < 20; i++) {           /* mode change is not instant */
    if ((readReg(REG_CANSTAT) & MODE_MASK) == mode) return true;
    delay(1);
  }
  return false;
}

/* -------------------------------------------------------------------------
 *  Bit timing.
 *
 *  Both tables below produce 16 time quanta per bit with the sample point at
 *  10/16 = 62.5 %, which is what CANopen and J1939 tooling expects:
 *      SYNC 1 + PROP 2 + PS1 7 + PS2 6 = 16 TQ
 *
 *  8 MHz : TQ = 2*(BRP+1)/8 MHz  = 250 ns -> 16 TQ = 4 us = 250 kbit/s
 *  16 MHz: TQ = 2*(BRP+1)/16 MHz = 250 ns -> 16 TQ = 4 us = 250 kbit/s
 * ---------------------------------------------------------------------- */
bool MCP2515::setBitrate(uint16_t kbps, uint8_t crystalMHz) {
  uint8_t cnf1, cnf2, cnf3;

  if (crystalMHz == 8) {
    switch (kbps) {
      case 1000: cnf1 = 0x00; cnf2 = 0x80; cnf3 = 0x00; break;
      case  500: cnf1 = 0x00; cnf2 = 0x90; cnf3 = 0x02; break;
      case  250: cnf1 = 0x00; cnf2 = 0xB1; cnf3 = 0x05; break;
      case  125: cnf1 = 0x01; cnf2 = 0xB1; cnf3 = 0x05; break;
      case  100: cnf1 = 0x01; cnf2 = 0xB4; cnf3 = 0x06; break;
      default: return false;
    }
  } else if (crystalMHz == 16) {
    switch (kbps) {
      case 1000: cnf1 = 0x00; cnf2 = 0xD0; cnf3 = 0x82; break;
      case  500: cnf1 = 0x00; cnf2 = 0xF0; cnf3 = 0x86; break;
      case  250: cnf1 = 0x41; cnf2 = 0xF1; cnf3 = 0x85; break;
      case  125: cnf1 = 0x03; cnf2 = 0xF0; cnf3 = 0x86; break;
      case  100: cnf1 = 0x03; cnf2 = 0xFA; cnf3 = 0x87; break;
      default: return false;
    }
  } else {
    return false;
  }

  writeReg(REG_CNF1, cnf1);
  writeReg(REG_CNF2, cnf2);
  writeReg(REG_CNF3, cnf3);
  return true;
}

bool MCP2515::begin(uint16_t bitrateKbps, uint8_t crystalMHz) {
  pinMode(_cs, OUTPUT);
  deselect();

  reset();

  /* After reset the chip must be in configuration mode. If it is not, the
   * chip is not answering at all - almost always wiring, CS or 3V3 power. */
  if ((readReg(REG_CANSTAT) & MODE_MASK) != MODE_CONFIG) return false;

  /* Prove the SPI link both ways before trusting anything else. */
  writeReg(REG_CNF1, 0x55);
  if (readReg(REG_CNF1) != 0x55) return false;
  writeReg(REG_CNF1, 0xAA);
  if (readReg(REG_CNF1) != 0xAA) return false;

  if (!setBitrate(bitrateKbps, crystalMHz)) return false;

  /* Receive everything: RXM = 11 disables the acceptance filters entirely. A
   * logger that filters is a logger that lies about what was on the bus.
   * BUKT on RXB0 lets an overflowing RXB0 roll into RXB1, which doubles the
   * time available to service an interrupt before the chip drops a frame. */
  writeReg(REG_RXB0CTRL, 0x64);
  writeReg(REG_RXB1CTRL, 0x60);

  /* Interrupt on either receive buffer, plus the error/message-error lines so
   * bus problems are visible instead of silent. */
  writeReg(REG_CANINTE, INT_RX0 | INT_RX1 | INT_ERR | INT_MERR);
  writeReg(REG_CANINTF, 0x00);

  /* Stays in configuration mode - silent, receiving nothing - until
   * startReceiving() is called. */
  return true;
}

bool MCP2515::startReceiving(bool listenOnly) {
  /* Discard anything that leaked in and clear every flag, so the first frame
   * the reader sees is genuinely the first frame on the bus. */
  writeReg(REG_CANINTF, 0x00);
  modifyReg(REG_EFLG, EFLG_RX0OVR | EFLG_RX1OVR, 0x00);

  /* Arm one-shot before the bus opens, so the first Send behaves like every
   * later one. It changes nothing about reception; see the note in the header
   * for why retrying forever is the dangerous default. */
  modifyReg(REG_CANCTRL, CANCTRL_OSM, CANCTRL_OSM);

  return setMode(listenOnly ? MODE_LISTENONLY : MODE_NORMAL);
}

uint8_t MCP2515::interruptFlags() { return readReg(REG_CANINTF); }

uint8_t MCP2515::clearErrorInterrupts() {
  const uint8_t sticky = readReg(REG_CANINTF) & (uint8_t)~(INT_RX0 | INT_RX1);
  if (sticky) {
    /* Bit-modify rather than a plain write: a receive buffer may fill between
     * the read and the write, and clobbering its flag would strand the frame
     * in the controller with no interrupt to announce it. */
    modifyReg(REG_CANINTF, sticky, 0x00);
  }
  return sticky;
}

bool MCP2515::framePending() {
  return (readStatus() & (INT_RX0 | INT_RX1)) != 0;
}

bool MCP2515::readFrame(CanFrame &out) {
  uint8_t st = readStatus();
  uint8_t cmd;

  if      (st & INT_RX0) cmd = CMD_READ_RX0;
  else if (st & INT_RX1) cmd = CMD_READ_RX1;
  else                   return false;

  /* One burst: SIDH, SIDL, EID8, EID0, DLC, D0..D7. Terminating the
   * transaction also clears the matching RXnIF flag in hardware. */
  uint8_t b[13];
  _spi.beginTransaction(_cfg);
  select();
  _spi.transfer(cmd);
  for (uint8_t i = 0; i < 13; i++) b[i] = _spi.transfer(0x00);
  deselect();
  _spi.endTransaction();

  const uint8_t sidh = b[0], sidl = b[1], dlc = b[4];

  if (sidl & 0x08) {                       /* extended identifier */
    out.ext = 1;
    out.id  = ((uint32_t)sidh << 21) |
              ((uint32_t)(sidl & 0xE0) << 13) |
              ((uint32_t)(sidl & 0x03) << 16) |
              ((uint32_t)b[2] << 8) | b[3];
    out.rtr = (dlc & 0x40) ? 1 : 0;
  } else {
    out.ext = 0;
    out.id  = ((uint32_t)sidh << 3) | (sidl >> 5);
    out.rtr = (sidl & 0x10) ? 1 : 0;
  }

  out.len = dlc & 0x0F;
  if (out.len > 8) out.len = 8;
  for (uint8_t i = 0; i < 8; i++) out.data[i] = b[5 + i];

  return true;
}

uint8_t MCP2515::takeRxOverflow() {
  uint8_t eflg = readReg(REG_EFLG) & (EFLG_RX0OVR | EFLG_RX1OVR);
  if (eflg) modifyReg(REG_EFLG, EFLG_RX0OVR | EFLG_RX1OVR, 0x00);
  return eflg;
}

uint8_t MCP2515::txErrorCount() { return readReg(REG_TEC); }
uint8_t MCP2515::rxErrorCount() { return readReg(REG_REC); }
uint8_t MCP2515::errorFlags()   { return readReg(REG_EFLG); }
uint8_t MCP2515::mode()         { return readReg(REG_CANSTAT) >> 5; }

/* ==========================================================================
 *  Transmit
 * ======================================================================== */
void MCP2515::abortTx() {
  /* Clearing TXREQ is the documented abort for a single buffer. CANCTRL's
   * ABAT would work too but it aborts every buffer, and on a controller that
   * only ever uses buffer 0 the narrower operation is the honest one. */
  modifyReg(REG_TXB0CTRL, TXB_TXREQ, 0x00);
}

bool MCP2515::canTransmit() {
  const uint8_t m = mode();
  return m == 0 /* normal */ || m == 2 /* loopback */;
}

MCP2515::TxResult MCP2515::sendFrame(const CanFrame &f, uint8_t attempts,
                                     int16_t *tecDelta) {
  if (tecDelta) *tecDelta = 0;

  if (f.len > 8) return TX_BAD_FRAME;
  if (f.ext) { if (f.id > 0x1FFFFFFFul) return TX_BAD_FRAME; }
  else       { if (f.id > 0x7FFul)      return TX_BAD_FRAME; }

  /* Listen-only is a deliberate configuration, not a fault: the logger is
   * often the only silent node on a live machine. Refusing here, loudly, is
   * better than a Send button that appears to work and does nothing. */
  if (!canTransmit()) return TX_NOT_LISTENING;

  if (readReg(REG_TXB0CTRL) & TXB_TXREQ) return TX_BUSY;

  /* SIDH, SIDL, EID8, EID0, DLC, D0..D7 - the same layout readFrame() parses,
   * built the other way round. */
  uint8_t b[13];
  if (f.ext) {
    b[0] = (uint8_t)(f.id >> 21);
    b[1] = (uint8_t)(((f.id >> 13) & 0xE0) | 0x08 | ((f.id >> 16) & 0x03));
    b[2] = (uint8_t)(f.id >> 8);
    b[3] = (uint8_t)(f.id);
  } else {
    b[0] = (uint8_t)(f.id >> 3);
    b[1] = (uint8_t)((f.id & 0x07) << 5);
    b[2] = 0;
    b[3] = 0;
  }
  b[4] = (uint8_t)((f.rtr ? 0x40 : 0x00) | (f.len & 0x0F));
  for (uint8_t i = 0; i < 8; i++) b[5 + i] = (i < f.len) ? f.data[i] : 0x00;

  if (attempts == 0) attempts = 1;
  const uint8_t tecBefore = readReg(REG_TEC);
  TxResult last = TX_TIMEOUT;

  for (uint8_t attempt = 0; attempt < attempts; attempt++) {
    /* The status bits are sticky across attempts, so they have to go before
     * each one or the second attempt reads the first one's verdict. */
    modifyReg(REG_TXB0CTRL, TXB_ABTF | TXB_MLOA | TXB_TXERR, 0x00);

    _spi.beginTransaction(_cfg);
    select();
    _spi.transfer(CMD_LOAD_TX0);
    for (uint8_t i = 0; i < 13; i++) _spi.transfer(b[i]);
    deselect();
    _spi.endTransaction();

    _spi.beginTransaction(_cfg);
    select();
    _spi.transfer(CMD_RTS_TX0);
    deselect();
    _spi.endTransaction();

    /* Bounded by iterations rather than by a clock: the longest classical CAN
     * frame at the slowest bit rate this driver supports is about 1.5 ms, so
     * 200 x 100 us is a wide margin, and a fixed count keeps the loop
     * deterministic on the host where micros() does not advance on its own. */
    uint8_t ctrl = 0;
    bool    done = false;
    for (uint16_t i = 0; i < 200; i++) {
      ctrl = readReg(REG_TXB0CTRL);
      if (!(ctrl & TXB_TXREQ)) { done = true; break; }
      delayMicroseconds(100);
    }

    if (!done) {
      /* One-shot should make this unreachable. If it happens the controller is
       * wedged, and leaving TXREQ set would block every future send. */
      abortTx();
      last = TX_TIMEOUT;
      break;
    }

    if (ctrl & TXB_MLOA) {
      /* Normal on a busy bus, and the one case worth retrying: a higher
       * priority identifier simply got there first. */
      last = TX_ARB_LOST;
      continue;
    }
    if (ctrl & TXB_TXERR) {
      /* An error during transmission. On a bus with no other node powered
       * this is the acknowledgement slot going unfilled, which is by far the
       * most common reason a Send does not work, so it gets its own answer
       * rather than a generic "bus error". */
      last = TX_NO_ACK;
      break;
    }
    if (ctrl & TXB_ABTF) { last = TX_ABORTED; break; }

    last = TX_OK;
    break;
  }

  const uint8_t tecAfter = readReg(REG_TEC);
  if (tecDelta) *tecDelta = (int16_t)tecAfter - (int16_t)tecBefore;

  /* The error counter is the independent witness. A frame that was genuinely
   * acknowledged decrements TEC; one that was not adds 8. If the status bits
   * said success and TEC climbed anyway, believe TEC. */
  if (last == TX_OK && tecAfter > tecBefore) last = TX_NO_ACK;

  return last;
}
