#include "storage.h"

#include <string>
#include <sys/stat.h>

#include "esp_display_panel.hpp"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "esp_spiffs.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"

namespace storage {
namespace {

const char *TAG = "storage";
const char *SPIFFS_BASE_PATH = "/webapp";
const char *SD_BASE_PATH = "/sdcard";

WebRootSource s_source = WebRootSource::None;
bool s_sd_present = false;

///////////////////////////////////////////////////////////////////////////
// SD card support
///////////////////////////////////////////////////////////////////////////
//
// On this board the TF/SD card's SPI bus shares the CH422G IO-expander for
// its chip-select line (SD_CS = CH422G EXIO4, per Waveshare's wiki/demo
// notes), and since nothing else lives on that SPI bus, CS is simply held
// asserted (driven low) for the whole session instead of being toggled per
// SPI transaction. The actual native GPIOs used for MOSI/MISO/SCK depend on
// exact board revision/silkscreen; verify them against your unit's schematic
// (https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-7/ESP32-S3-Touch-LCD-7-Sch.pdf)
// before enabling this.
//
// Disabled by default so a fresh checkout always builds & boots using the
// internal SPIFFS copy of the web UI; flip STORAGE_SD_ENABLED to 1 and fill
// in the pins below once verified.
#define STORAGE_SD_ENABLED 0

#if STORAGE_SD_ENABLED
constexpr gpio_num_t SD_PIN_MOSI = GPIO_NUM_11;
constexpr gpio_num_t SD_PIN_MISO = GPIO_NUM_13;
constexpr gpio_num_t SD_PIN_SCLK = GPIO_NUM_12;
constexpr spi_host_device_t SD_SPI_HOST = SPI2_HOST;
// EXIO index on the CH422G used for SD_CS (per Waveshare notes: EXIO4).
constexpr uint8_t SD_CS_EXIO_PIN = 4;

bool assertSdChipSelect(esp_panel::board::Board *board)
{
    if (board == nullptr || board->getIO_Expander() == nullptr) {
        ESP_LOGW(TAG, "No IO expander available, cannot assert SD_CS");
        return false;
    }
    auto *expander = board->getIO_Expander()->getBase();
    if (expander == nullptr) {
        return false;
    }
    // NOTE: verify pinMode()/digitalWrite() signatures against the installed
    // esp32_io_expander version (these are not Arduino macros here); adjust
    // the direction/level constants below if the API differs.
    constexpr int kOutput = 1;
    constexpr int kLow = 0;
    expander->pinMode(SD_CS_EXIO_PIN, kOutput);
    expander->digitalWrite(SD_CS_EXIO_PIN, kLow);
    return true;
}

bool mountSdCard(esp_panel::board::Board *board)
{
    if (!assertSdChipSelect(board)) {
        return false;
    }

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = SD_PIN_MOSI;
    bus_cfg.miso_io_num = SD_PIN_MISO;
    bus_cfg.sclk_io_num = SD_PIN_SCLK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 4000;
    if (spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize SD SPI bus");
        return false;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = GPIO_NUM_NC; // CS is held low externally via the IO expander
    slot_cfg.host_id = SD_SPI_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 5;

    sdmmc_card_t *card = nullptr;
    esp_err_t err = esp_vfs_fat_sdspi_mount(SD_BASE_PATH, &host, &slot_cfg, &mount_config, &card);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No SD card mounted (%s) -- using internal storage", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "SD card mounted at %s", SD_BASE_PATH);
    return true;
}
#endif // STORAGE_SD_ENABLED

bool pathHasIndexHtml(const char *base_path)
{
    std::string index_path = std::string(base_path) + "/index.html";
    struct stat st;
    return stat(index_path.c_str(), &st) == 0;
}

bool mountSpiffs()
{
    esp_vfs_spiffs_conf_t conf = {};
    conf.base_path = SPIFFS_BASE_PATH;
    conf.partition_label = "webapp";
    conf.max_files = 10;
    conf.format_if_mount_failed = true;

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SPIFFS webapp partition: %s", esp_err_to_name(err));
        return false;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info("webapp", &total, &used);
    ESP_LOGI(TAG, "SPIFFS webapp mounted: %d/%d KB used", (int)(used / 1024), (int)(total / 1024));
    return true;
}

} // namespace

void init(esp_panel::board::Board *board)
{
    bool spiffs_ok = mountSpiffs();

#if STORAGE_SD_ENABLED
    s_sd_present = mountSdCard(board);
#else
    (void)board;
    s_sd_present = false;
#endif

    if (s_sd_present && pathHasIndexHtml(SD_BASE_PATH)) {
        s_source = WebRootSource::SdCard;
        ESP_LOGI(TAG, "Serving web UI from SD card");
    } else if (spiffs_ok && pathHasIndexHtml(SPIFFS_BASE_PATH)) {
        s_source = WebRootSource::Spiffs;
        ESP_LOGI(TAG, "Serving web UI from internal SPIFFS");
    } else {
        s_source = WebRootSource::None;
        ESP_LOGW(TAG, "No web UI bundle found on SPIFFS or SD card!");
    }
}

WebRootSource webRootSource()
{
    return s_source;
}

const char *webRootPath()
{
    switch (s_source) {
    case WebRootSource::SdCard:
        return SD_BASE_PATH;
    case WebRootSource::Spiffs:
        return SPIFFS_BASE_PATH;
    default:
        return SPIFFS_BASE_PATH;
    }
}

bool sdCardPresent()
{
    return s_sd_present;
}

} // namespace storage
