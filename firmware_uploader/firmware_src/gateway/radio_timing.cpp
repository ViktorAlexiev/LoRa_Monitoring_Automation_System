#include "radio_timing.h"

static uint8_t  s_sf  = RADIO_SF_DEFAULT;
static uint32_t s_bw  = RADIO_BW_DEFAULT_HZ;
static uint8_t  s_cr  = 5;      // знаменател: 5..8 (4/5..4/8)
static uint8_t  s_pre = 8;
static bool     s_crc = true;

static const uint32_t BW_TABLE[RADIO_BW_INDEX_COUNT] = {62500UL, 125000UL, 250000UL};

bool radioSfValid(uint8_t sf) {
  return sf >= RADIO_SF_MIN && sf <= RADIO_SF_MAX;
}

uint32_t radioBwFromIndex(uint8_t idx) {
  return idx < RADIO_BW_INDEX_COUNT ? BW_TABLE[idx] : 0;
}

uint8_t radioBwToIndex(uint32_t bwHz) {
  for (uint8_t i = 0; i < RADIO_BW_INDEX_COUNT; i++) {
    if (BW_TABLE[i] == bwHz) return i;
  }
  return 0xFF;
}

void radioTimingInit(uint8_t sf, uint32_t bwHz, uint8_t crDenom, uint8_t preambleLen, bool crcOn) {
  s_sf  = radioSfValid(sf) ? sf : RADIO_SF_DEFAULT;
  s_bw  = (radioBwToIndex(bwHz) != 0xFF) ? bwHz : RADIO_BW_DEFAULT_HZ;
  s_cr  = (crDenom >= 5 && crDenom <= 8) ? crDenom : 5;
  s_pre = preambleLen;
  s_crc = crcOn;
}

uint8_t  radioSf()          { return s_sf; }
uint32_t radioBw()          { return s_bw; }
uint8_t  radioCrDenom()     { return s_cr; }
uint8_t  radioPreambleLen() { return s_pre; }

// Ts = 2^SF / BW. Най-лошия случай 2^12 * 1e6 = 4.096e9 - събира се в uint32 (макс 4.29e9).
uint32_t radioSymbolUs() {
  return ((1UL << s_sf) * 1000000UL) / s_bw;
}

bool radioLdroNeeded() {
  return radioSymbolUs() > 16000UL;
}

uint32_t radioToaMs(uint8_t payloadLen) {
  uint32_t ts = radioSymbolUs();
  int32_t de  = radioLdroNeeded() ? 2 : 0;
  // explicit header (IH=0): num = 8*PL - 4*SF + 28 + 16*CRC
  int32_t num = 8L * payloadLen - 4L * s_sf + 28 + (s_crc ? 16 : 0);
  int32_t den = 4L * ((int32_t)s_sf - de);
  int32_t blocks = num > 0 ? (num + den - 1) / den : 0;
  uint32_t nSym = 8UL + (uint32_t)blocks * s_cr;   // (CR+4) == знаменателя 5..8
  // преамбюл: (n + 4.25) * Ts = (4n + 17) * Ts / 4
  uint32_t preUs = ((4UL * s_pre + 17UL) * ts) / 4UL;
  uint32_t totalUs = preUs + nSym * ts;
  return (totalUs + 999UL) / 1000UL;
}

// Един CAD трае ~ (2^SF + 32) / BW; вземаме двоен запас за SPI/polling + 5 ms.
uint32_t radioCadTimeoutMs() {
  uint32_t cadUs = (((1UL << s_sf) + 32UL) * 1000000UL) / s_bw;
  return (2UL * cadUs + 999UL) / 1000UL + 5UL;
}

uint32_t radioCadStepMs() {
  uint32_t t = radioCadTimeoutMs();
  uint32_t fourTs = (4UL * radioSymbolUs() + 999UL) / 1000UL;
  return t > fourTs ? t : fourTs;
}

uint32_t radioSensingWindowMs() { return radioToaMs(RADIO_MAX_PAYLOAD_LEN); }
uint32_t radioTelemetryWaitMs() { return radioToaMs(RADIO_TELEMETRY_LEN); }
uint32_t radioSlotMs()          { return radioCadStepMs(); }

// (3 * x + 1) / 2 == ceil(1.5 * x)
static uint32_t withMargin(uint32_t x) { return (3UL * x + 1UL) / 2UL; }

// команда + пауза на Executor + ACK, x1.5, плюс по един срок за сондиране от двете страни
uint32_t radioAckTimeoutCmdMs() {
  uint32_t base = radioToaMs(RADIO_CMD_WIRE_LEN) + RADIO_EXEC_ACK_DELAY_MS + radioToaMs(RADIO_ACK_WIRE_LEN);
  return withMargin(base) + 2UL * radioSensingWindowMs();
}

uint32_t radioAckTimeoutRestartMs() {
  uint32_t base = radioToaMs(RADIO_MAX_PAYLOAD_LEN) + RADIO_PROCESSING_MS + radioToaMs(RADIO_ACK_WIRE_LEN);
  return withMargin(base) + 2UL * radioSensingWindowMs();
}

uint32_t radioStateReqTimeoutMs() {
  uint32_t base = radioToaMs(RADIO_ACK_WIRE_LEN) + RADIO_PROCESSING_MS + radioToaMs(RADIO_MAX_PAYLOAD_LEN);
  return withMargin(base) + 2UL * radioSensingWindowMs();
}
