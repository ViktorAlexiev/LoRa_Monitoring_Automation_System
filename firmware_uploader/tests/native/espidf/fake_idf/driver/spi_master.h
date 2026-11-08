#ifndef FAKE_DRIVER_SPI_MASTER_H
#define FAKE_DRIVER_SPI_MASTER_H

// Заместител на driver/spi_master.h (ESP-IDF 5.x). Полетата са надмножество на нужните на RadioLib EspHal и на прошивките.
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#define SOC_SPI_MAXIMUM_BUFFER_SIZE 64

typedef enum { SPI1_HOST = 0, SPI2_HOST = 1, SPI3_HOST = 2 } spi_host_device_t;
typedef enum { SPI_DMA_DISABLED = 0, SPI_DMA_CH_AUTO = 3 } spi_common_dma_t;
typedef enum { ESP_INTR_CPU_AFFINITY_AUTO = 0, ESP_INTR_CPU_AFFINITY_0 = 1, ESP_INTR_CPU_AFFINITY_1 = 2 } esp_intr_cpu_affinity_t;
typedef enum { SPI_CLK_SRC_DEFAULT = 0 } spi_clock_source_t;

typedef struct {
  int mosi_io_num;
  int miso_io_num;
  int sclk_io_num;
  int quadwp_io_num;
  int quadhd_io_num;
  int data4_io_num;
  int data5_io_num;
  int data6_io_num;
  int data7_io_num;
  int max_transfer_sz;
  uint32_t flags;
  esp_intr_cpu_affinity_t isr_cpu_id;
  int intr_flags;
} spi_bus_config_t;

typedef struct {
  uint8_t command_bits;
  uint8_t address_bits;
  uint8_t dummy_bits;
  uint8_t mode;
  spi_clock_source_t clock_source;
  uint16_t duty_cycle_pos;
  uint16_t cs_ena_pretrans;
  uint8_t cs_ena_posttrans;
  int clock_speed_hz;
  int input_delay_ns;
  int spics_io_num;
  uint32_t flags;
  int queue_size;
  void (*pre_cb)(void *);
  void (*post_cb)(void *);
} spi_device_interface_config_t;

#define SPI_TRANS_USE_RXDATA (1 << 2)
#define SPI_TRANS_USE_TXDATA (1 << 3)

typedef struct {
  uint32_t flags;
  size_t length;      // в БИТОВЕ
  union { const void *tx_buffer; uint8_t tx_data[4]; };
  union { void *rx_buffer; uint8_t rx_data[4]; };
} spi_transaction_t;

typedef void *spi_device_handle_t;

esp_err_t spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t *cfg, int dma);
esp_err_t spi_bus_free(spi_host_device_t host);
esp_err_t spi_bus_add_device(spi_host_device_t host, const spi_device_interface_config_t *cfg, spi_device_handle_t *h);
esp_err_t spi_bus_remove_device(spi_device_handle_t h);
esp_err_t spi_device_acquire_bus(spi_device_handle_t h, uint32_t wait);
void spi_device_release_bus(spi_device_handle_t h);
esp_err_t spi_device_polling_transmit(spi_device_handle_t h, spi_transaction_t *t);

#endif
