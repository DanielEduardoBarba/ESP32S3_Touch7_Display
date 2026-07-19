#pragma once

/**
 * Compile-time enables for the peer-to-peer communication transports.
 *
 * All four transports are fully implemented in ports.cpp, but only the ones
 * enabled here are initialized and offered in the Ports scene (ESP GUI and
 * web GUI). RS485 is the product default; flip a 0 to 1 to light up
 * another transport -- no other code changes needed.
 *
 * Board wiring facts (from Waveshare's official demos, same on 7 and 7B):
 *   RS485:  UART1, TX=GPIO16, RX=GPIO15 (auto-direction transceiver)
 *   CAN:    TWAI,  TX=GPIO20, RX=GPIO19 -- on the 7B the transceiver is
 *           behind the IO EXTENSION chip (0x24), IO5 must be driven HIGH
 *           (0=USB, 1=CAN); ports.cpp does that when CAN is enabled.
 *   UART:   UART2 on the PH2.0 header, TX=GPIO43, RX=GPIO44. NOTE: those
 *           are also the default console pins -- if you enable this,
 *           consider moving the console (sdkconfig) or accept mixed output.
 *   I2C:    the PH2.0 I2C header shares the touch/IO-extension bus
 *           (SCL=GPIO9, SDA=GPIO8). The link runs as a master writing to a
 *           fixed peer address, so both boards must not talk simultaneously
 *           -- fine for the request/response protocol, but half-duplex.
 */
#define PORTS_ENABLE_RS485  1
#define PORTS_ENABLE_CAN    0
#define PORTS_ENABLE_I2C    0
#define PORTS_ENABLE_UART   0
