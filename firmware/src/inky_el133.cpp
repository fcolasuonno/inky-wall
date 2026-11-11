#include "inky_el133.h"
#include "config.h"

static inline void csWrite(uint8_t pin, bool low) { digitalWrite(pin, low ? LOW : HIGH); }

bool InkyEL133::begin() {
    SPI.begin(PIN_SCK, -1, PIN_MOSI, -1);
    pinMode(PIN_CS0, OUTPUT); csWrite(PIN_CS0, false);
    pinMode(PIN_CS1, OUTPUT); csWrite(PIN_CS1, false);
    pinMode(PIN_DC,  OUTPUT); digitalWrite(PIN_DC, LOW);
    pinMode(PIN_RST, OUTPUT); digitalWrite(PIN_RST, LOW);
    pinMode(PIN_BUSY, INPUT_PULLUP);
    delay(10);
    return true;
}

void InkyEL133::cmd(uint8_t command, uint8_t csSel, const uint8_t* data, size_t len) {
    SPI.beginTransaction(_spi);
    if (csSel & CS0_SEL) csWrite(PIN_CS0, true);
    if (csSel & CS1_SEL) csWrite(PIN_CS1, true);

    digitalWrite(PIN_DC, LOW);
    SPI.transfer(command);

    if (data && len) {
        digitalWrite(PIN_DC, HIGH);
        // chunk the data like the python driver (spidev 4096-byte writes)
        while (len) {
            size_t chunk = len > 4096 ? 4096 : len;
            SPI.transferBytes(data, nullptr, chunk);
            data += chunk;
            len -= chunk;
        }
    }
    csWrite(PIN_CS0, false);
    csWrite(PIN_CS1, false);
    digitalWrite(PIN_DC, LOW);
    SPI.endTransaction();
}

void InkyEL133::dataChunk(const uint8_t* p, size_t n) {
    SPI.transferBytes(p, nullptr, n);
}

// BUSY is active-LOW: LOW == busy, HIGH == ready
void InkyEL133::busyWait(uint32_t timeout_s) {
    uint32_t t0 = millis();
    while (digitalRead(PIN_BUSY) == LOW) {
        if (millis() - t0 > timeout_s * 1000UL) {
            Serial.printf("[inky] busyWait timed out after %us\n", (unsigned)timeout_s);
            return;
        }
        delay(5);
    }
}

// after DRF: wait for BUSY to assert (drop LOW), then wait for release.
// Sending POF mid-refresh latches panels into a fault state!
void InkyEL133::refreshWait(uint32_t timeout_s) {
    uint32_t t0 = millis();
    while (digitalRead(PIN_BUSY) == HIGH) {
        if (millis() - t0 > timeout_s * 1000UL) {
            Serial.printf("[inky] refreshWait (assert) timed out\n");
            return;
        }
        delay(5);
    }
    while (digitalRead(PIN_BUSY) == LOW) {
        if (millis() - t0 > timeout_s * 1000UL) {
            Serial.printf("[inky] refreshWait (release) timed out\n");
            return;
        }
        delay(20);
    }
}

void InkyEL133::powerUp() {
    digitalWrite(PIN_RST, LOW);
    delay(30);
    digitalWrite(PIN_RST, HIGH);
    delay(300);
    busyWait(1);

    static const uint8_t ANTM[] = {0x00, 0x0C, 0x0C, 0xD9, 0xDD, 0xDD, 0x15, 0x15, 0x55};
    static const uint8_t CMD66[] = {0x49, 0x55, 0x13, 0x5D, 0x05, 0x10};
    static const uint8_t PSR[]   = {0xDF, 0x6B};
    static const uint8_t DCDC[]  = {0x44, 0x54, 0x00};
    static const uint8_t POFS_A[] = {0x00, 0xC0, 0x03, 0xA8};
    static const uint8_t POFS_B[] = {0x00, 0xC0, 0x03, 0x9A};
    static const uint8_t CDI[]   = {0x37};
    static const uint8_t TCON[]  = {0x03, 0x03};
    static const uint8_t AGID[]  = {0x10};
    static const uint8_t PWS[]   = {0x22};
    static const uint8_t CCSET[] = {0x01};
    static const uint8_t TRES[]  = {0x04, 0xB0, 0x03, 0x20};
    static const uint8_t CMDA4[]  = {0x03, 0x00, 0x01, 0x03, 0x00, 0x03, 0x00, 0x00, 0x00};
    static const uint8_t PWR[]    = {0x0F, 0x00, 0x28, 0x2C, 0x28, 0x38};
    static const uint8_t EN_BUF[] = {0x07};
    static const uint8_t BTST[]   = {0xE0, 0x20};
    static const uint8_t ON[]     = {0x01};
    static const uint8_t VCOM[]   = {0x02};

    cmd(0x74, CS0_SEL, ANTM, sizeof(ANTM));   // ANTM (analog test mode)
    cmd(0xF0, CS_BOTH, CMD66, sizeof(CMD66)); // CMD66
    cmd(0x00, CS_BOTH, PSR, sizeof(PSR));     // PSR
    cmd(0xA5, CS0_SEL, DCDC, sizeof(DCDC));   // DCDC
    cmd(0x30, CS_BOTH, &_speed, 1);           // PLL / speed
    cmd(0x50, CS_BOTH, CDI, sizeof(CDI));     // CDI
    cmd(0x60, CS_BOTH, TCON, sizeof(TCON));   // TCON
    cmd(0x03, CS0_SEL, POFS_A, sizeof(POFS_A));
    cmd(0x03, CS1_SEL, POFS_B, sizeof(POFS_B));
    cmd(0x86, CS_BOTH, AGID, sizeof(AGID));   // AGID
    cmd(0xE3, CS_BOTH, PWS, sizeof(PWS));     // PWS
    cmd(0xE0, CS_BOTH, CCSET, sizeof(CCSET)); // CCSET
    cmd(0x61, CS_BOTH, TRES, sizeof(TRES));   // TRES 1200x800/controller
    cmd(0xA4, CS0_SEL, CMDA4, sizeof(CMDA4)); // CMDA4
    cmd(0x01, CS0_SEL, PWR, sizeof(PWR));     // PWR
    cmd(0xB6, CS0_SEL, EN_BUF, sizeof(EN_BUF));
    cmd(0x06, CS0_SEL, BTST, sizeof(BTST));   // BTST_P
    cmd(0xB7, CS0_SEL, ON, sizeof(ON));       // BOOST_VDDP_EN
    cmd(0x05, CS0_SEL, BTST, sizeof(BTST));   // BTST_N
    cmd(0xB0, CS0_SEL, ON, sizeof(ON));       // BUCK_BOOST_VDDN
    cmd(0xB1, CS0_SEL, VCOM, sizeof(VCOM));   // TFT_VCOM_POWER
}

// Tesserae esp32_bin layout: one continuous 4bpp stream, 600 bytes per row
// (1200 px portrait scan), left 300 bytes belong to CS0 and the right 300 to
// CS1. Both data windows are opened once, then each row is split on the fly so
// the whole frame crosses the network only once (battery: no second download,
// no framebuffer).
bool InkyEL133::streamSplit(Client& src, size_t total, uint32_t timeout_ms) {
    static constexpr size_t ROW = 600;   // PANEL_SCAN_W / 2
    static constexpr size_t HALF = 300;  // one controller's share of a row
    static uint8_t rowBuf[ROW];

    SPI.beginTransaction(_spi);

    // open the pixel-data window on both controllers, then leave DC high
    csWrite(PIN_CS0, true);
    digitalWrite(PIN_DC, LOW);  SPI.transfer(0x10); digitalWrite(PIN_DC, HIGH);
    csWrite(PIN_CS0, false);
    csWrite(PIN_CS1, true);
    digitalWrite(PIN_DC, LOW);  SPI.transfer(0x10); digitalWrite(PIN_DC, HIGH);
    csWrite(PIN_CS1, false);

    size_t got = 0;
    uint32_t t0 = millis();
    bool ok = true;
    while (got < total) {
        size_t want = total - got;
        if (want > ROW) want = ROW;

        size_t filled = 0;
        while (filled < want) {
            int n = src.read(rowBuf + filled, want - filled);
            if (n > 0) {
                filled += (size_t)n;
                t0 = millis();
            } else if (millis() - t0 > timeout_ms) {
                ok = false;
                break;
            } else {
                delay(2);
            }
        }
        if (!ok) break;

        csWrite(PIN_CS0, true);
        SPI.transferBytes(rowBuf, nullptr, HALF);
        csWrite(PIN_CS0, false);

        csWrite(PIN_CS1, true);
        SPI.transferBytes(rowBuf + HALF, nullptr, HALF);
        csWrite(PIN_CS1, false);

        got += want;
        if ((got % 120000) < ROW) {
            Serial.printf("[inky] split %uKB/%uKB\n",
                          (unsigned)(got / 1000), (unsigned)(total / 1000));
        }
    }

    SPI.endTransaction();
    csWrite(PIN_CS0, false);
    csWrite(PIN_CS1, false);
    digitalWrite(PIN_DC, LOW);

    if (got != total) {
        Serial.printf("[inky] split stalled at %u/%u\n",
                      (unsigned)got, (unsigned)total);
    }
    return ok && got == total;
}

void InkyEL133::commit() {
    cmd(0x04, CS_BOTH, nullptr, 0);   // PON
    busyWait(1);

    static const uint8_t DRF[] = {0x00};
    cmd(0x12, CS_BOTH, DRF, sizeof(DRF));   // DRF
    refreshWait(65);

    static const uint8_t POF[] = {0x00};
    cmd(0x02, CS_BOTH, POF, sizeof(POF));   // POF
    busyWait(1);
}
