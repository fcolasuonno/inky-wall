#pragma once
#include <Arduino.h>
#include <SPI.h>

// Port of pimoroni/inky -> inky/inky_el133uf1.py (EL133UF1, 13.3" Spectra 6).
// Streaming variant: because boards without PSRAM cannot buffer a full frame,
// the driver pulls data straight onto the panel's SPI bus in 600-byte rows.
//
// Usage per update:
//    panel.begin();                    // SPI + pinMode init (once)
//    panel.powerUp();                  // reset + init command sequence
//    panel.streamSplit(src, 960000, HTTP_TIMEOUT_MS);
//    panel.commit();                   // PON/DRF/refresh/POF (~13-35 s)
// On an aborted stream, do NOT call commit(); the panel keeps the old image.

class InkyEL133 {
public:
    // PLL / frame-rate byte written to command 0x30. Slower refresh draws less
    // current and looks better on Spectra 6; 0x04 (~13 s) and 0x07 (~8 s) are
    // the faster settings if a future build ever wants them.
    static constexpr uint8_t SPEED_NORMAL = 0x08;  // ~19.5 s

    // chip selects: which controller(s) to address
    static constexpr uint8_t CS0_SEL = 1, CS1_SEL = 2, CS_BOTH = 3;

    bool begin();
    void setFrameSpeed(uint8_t speed) { _speed = speed; }

    void powerUp();   // reset pulse + full init command sequence
    // Tesserae layout: 600-byte rows (1200 px portrait), left 300 bytes to CS0,
    // right 300 bytes to CS1, one pass, no framebuffer. `total` must be
    // rows * 600 (960000 for the 13.3"). Returns true only on a full stream.
    bool streamSplit(Client& src, size_t total, uint32_t timeout_ms);
    void commit();    // finish the update (blocking call, 13-35 s)

private:
    void cmd(uint8_t command, uint8_t csSel, const uint8_t* data, size_t len);
    void dataChunk(const uint8_t* p, size_t n);
    void busyWait(uint32_t timeout_s);
    void refreshWait(uint32_t timeout_s);

    uint8_t _speed = SPEED_NORMAL;
    SPISettings _spi{10000000, MSBFIRST, SPI_MODE0};
};
