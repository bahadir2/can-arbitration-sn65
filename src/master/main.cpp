#include <Arduino.h>
#include "driver/twai.h"

// =================== MASTER (SN65HVD230 versiyonu) ===================
// TX_ID = 0x123 (dusuk ID -> arbitration'da HER ZAMAN kazanmasi beklenir)
// RX_ID = 0x456
// Artik SPI/MCP2515 yok - ESP32-C6'nin dahili TWAI controller'i dogrudan
// SN65HVD230 transceiver'ini suruyor.
// ========================================================================

#define TWAI_TX_PIN GPIO_NUM_0
#define TWAI_RX_PIN GPIO_NUM_1
#define TRIGGER_PIN 3     // Ortak tetik hatti - SLAVE ile ayni GPIO, ortak butona bagli

#define TX_ID     0x123
#define RX_ID     0x456
#define ROLE_NAME "MASTER"

// ---- Tetik ISR ----
volatile bool     triggerFlag   = false;
volatile uint32_t triggerMicros = 0;

void IRAM_ATTR onTrigger() {
  detachInterrupt(digitalPinToInterrupt(TRIGGER_PIN));
  triggerMicros = micros();
  triggerFlag = true;
}

// ---- Ayarlanabilir on-gecikme (terminalden + / - ile degistirilir) ----
volatile int32_t  extraDelayUs = 0;
const int32_t      DELAY_STEP_US = 2;
const int32_t      DELAY_MAX_US  = 5000;
const uint8_t       BURST_COUNT   = 4;   // her tetiklemede kac paket gonderilecek
const uint32_t      FIXED_INTERBURST_DELAY_US = 500;

// ---- Sayaclar ----
uint32_t roundNo = 0;
uint16_t globalSeq = 0;
uint32_t txOk = 0, txErrCallback = 0;
uint32_t rxOk = 0, rxWrongId = 0;
uint16_t lastRxSeq = 0xFFFF;
uint32_t lastArbLostCount = 0;

void drainIncoming();  // ileri bildirim

void printDelaySetting() {
  Serial.printf("[%s] >>> On-gecikme: %ld us  (+ arttir, - azalt, r sifirla)\n",
                ROLE_NAME, (long)extraDelayUs);
}

void handleSerialCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '+') {
      extraDelayUs = min(extraDelayUs + DELAY_STEP_US, DELAY_MAX_US);
      printDelaySetting();
    } else if (c == '-') {
      extraDelayUs = max(extraDelayUs - DELAY_STEP_US, (int32_t)0);
      printDelaySetting();
    } else if (c == 'r' || c == 'R') {
      extraDelayUs = 0;
      printDelaySetting();
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.printf("[%s] Arbitration jitter testi baslatiliyor (TX_ID=0x%03X RX_ID=0x%03X) - SN65HVD230/TWAI\n",
                ROLE_NAME, TX_ID, RX_ID);
  printDelaySetting();

  twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(TWAI_TX_PIN, TWAI_RX_PIN, TWAI_MODE_NORMAL);
  twai_timing_config_t  t_config = TWAI_TIMING_CONFIG_500KBITS();
  twai_filter_config_t  f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

  if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK) {
    Serial.printf("[%s] !!! TWAI driver kurulamadi !!!\n", ROLE_NAME);
  }
  if (twai_start() != ESP_OK) {
    Serial.printf("[%s] !!! TWAI baslatilamadi !!!\n", ROLE_NAME);
  }

  uint32_t alertsToEnable = TWAI_ALERT_ARB_LOST | TWAI_ALERT_TX_FAILED |
                            TWAI_ALERT_TX_SUCCESS | TWAI_ALERT_BUS_ERROR |
                            TWAI_ALERT_RX_DATA;
  twai_reconfigure_alerts(alertsToEnable, NULL);

  pinMode(TRIGGER_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TRIGGER_PIN), onTrigger, FALLING);

  twai_status_info_t status;
  twai_get_status_info(&status);
  lastArbLostCount = status.arb_lost_count;
  Serial.printf("[%s] Hazir. Baslangic arb_lost_count=%u\n", ROLE_NAME, (unsigned)lastArbLostCount);
}

void loop() {
  handleSerialCommands();

  // Buton birakilinca interrupt'i yeniden ac (debounce)
  static bool waitingForRelease = false;
  static uint32_t releaseCheckMs = 0;
  if (waitingForRelease && millis() - releaseCheckMs > 15) {
    releaseCheckMs = millis();
    if (digitalRead(TRIGGER_PIN) == HIGH) {
      waitingForRelease = false;
      attachInterrupt(digitalPinToInterrupt(TRIGGER_PIN), onTrigger, FALLING);
    }
  }

  if (triggerFlag) {
    triggerFlag = false;
    waitingForRelease = true;
    releaseCheckMs = millis();
    roundNo++;

    uint32_t t0 = micros();
    if (extraDelayUs > 0) delayMicroseconds(extraDelayUs);

    uint16_t mloaCountThisRound = 0;

    for (uint8_t burstIdx = 1; burstIdx <= BURST_COUNT; burstIdx++) {
      uint32_t tSend0 = micros();

      twai_message_t frame = {};
      frame.identifier = TX_ID;
      frame.data_length_code = 8;
      globalSeq++;
      frame.data[0] = (globalSeq >> 8) & 0xFF;
      frame.data[1] = globalSeq & 0xFF;
      frame.data[2] = roundNo & 0xFF;
      frame.data[3] = burstIdx;
      for (int i = 4; i < 8; i++) frame.data[i] = i;

      esp_err_t res = twai_transmit(&frame, pdMS_TO_TICKS(50));

      // KRITIK DUZELTME: twai_transmit() sadece kuyruga ekliyor, fiili
      // gonderimi beklemiyor. Bir sonraki mesaja gecmeden once BU mesajin
      // kuyruktan tamamen ciktigini (basarili ya da defalarca retry sonrasi)
      // dogrulamadan alert okursak, alert baska bir mesaja ait olabilir ve
      // yanlis burstIdx'e atfedilir (gozlemlenen off-by-one hatasi buydu).
      uint32_t alerts = 0;
      uint32_t accumulatedAlerts = 0;
      twai_status_info_t status;
      uint32_t waitStart = micros();
      do {
        twai_read_alerts(&alerts, pdMS_TO_TICKS(5));
        accumulatedAlerts |= alerts;
        twai_get_status_info(&status);
      } while (status.msgs_to_tx > 0 && (uint32_t)(micros() - waitStart) < 20000);

      bool arbLost  = accumulatedAlerts & TWAI_ALERT_ARB_LOST;
      bool txFailed = accumulatedAlerts & TWAI_ALERT_TX_FAILED;
      uint32_t dArbLost = status.arb_lost_count - lastArbLostCount;
      lastArbLostCount = status.arb_lost_count;

      uint32_t tSend1 = micros();

      if (res == ESP_OK && !arbLost) txOk++;
      else                           txErrCallback++;

      Serial.printf(
        "[%s] #%lu.%u SEQ=%u TRIG=%lu T0=%lu delayUs=%ld SEND0=%lu SEND1=%lu DUR=%luus RES=%d "
        "ARBLOST=%d TXFAILED=%d dARBLOST=%u TEC=%u REC=%u TXOK=%lu TXERRCB=%lu\n",
        ROLE_NAME, roundNo, burstIdx, globalSeq, triggerMicros, t0, (long)extraDelayUs, tSend0, tSend1,
        (unsigned long)(tSend1 - tSend0), res, arbLost, txFailed, (unsigned)dArbLost,
        (unsigned)status.tx_error_counter, (unsigned)status.rx_error_counter,
        txOk, txErrCallback);

      // ---- ARBITRATION YORUMU ----
      // MASTER dusuk ID (0x123) tasiyor -> gercek bir cakismada HER ZAMAN kazanmasi beklenir.
      if (arbLost) {
        mloaCountThisRound++;
        Serial.printf("[%s] *** BEKLENMEDIK: ARBITRATION KAYBEDILDI (dusuk ID kaybetti - anormal!) ***\n", ROLE_NAME);
      } else if (res == ESP_OK) {
        Serial.printf("[%s] >>> Gonderim basarili (cakisma yoktu ya da MASTER kazandi - beklenen durum)\n", ROLE_NAME);
      } else {
        Serial.printf("[%s] >>> Gonderim HATASI, arbitration disi bir sorun (res=%d)\n", ROLE_NAME, res);
      }

      delayMicroseconds(FIXED_INTERBURST_DELAY_US);
    }

    Serial.printf("[%s] ===== Round #%lu OZET: %u/%u gonderimde arbitration kaybi =====\n",
                  ROLE_NAME, roundNo, mloaCountThisRound, (unsigned)BURST_COUNT);
  }

  drainIncoming();
}

void drainIncoming() {
  twai_message_t rxFrame;
  while (twai_receive(&rxFrame, 0) == ESP_OK) {
    if (rxFrame.identifier == RX_ID) rxOk++;
    else                             rxWrongId++;

    uint16_t rxSeq = ((uint16_t)rxFrame.data[0] << 8) | rxFrame.data[1];
    bool isDuplicate = (rxSeq == lastRxSeq);
    lastRxSeq = rxSeq;

    twai_status_info_t status;
    twai_get_status_info(&status);

    Serial.printf("[%s] RX id=0x%03X SEQ=%u round=%u burst=%u RXOK=%lu RXWRONG=%lu rx_missed=%u rx_overrun=%u %s\n",
                  ROLE_NAME, (unsigned int)rxFrame.identifier, rxSeq, rxFrame.data[2], rxFrame.data[3],
                  rxOk, rxWrongId, (unsigned)status.rx_missed_count, (unsigned)status.rx_overrun_count,
                  isDuplicate ? "*** TEKRAR (ayni SEQ, muhtemelen donanim auto-retry) ***" : "");
  }
}