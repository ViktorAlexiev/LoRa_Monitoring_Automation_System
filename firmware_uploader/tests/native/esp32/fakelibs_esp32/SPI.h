#ifndef FAKE_SPI_H
#define FAKE_SPI_H
struct SPIClass { void begin(int = 0, int = 0, int = 0, int = 0) {} };
static SPIClass SPI;
#endif
