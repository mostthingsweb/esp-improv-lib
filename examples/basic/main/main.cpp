#include "improv_ble.h"

#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <nvs_flash.h>

namespace {
constexpr const char *TAG = "improv_example";

void init_nvs() {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void init_wifi() {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
}
} // namespace

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "Starting esp-improv-lib example...");
    init_nvs();
    init_wifi();

    // Your TODO: replace these strings as appropriate
    static improv_ble::ImprovBleService service({
        .firmware_name = "esp-improv-lib",
        .firmware_version = "0.1.0",
        .hardware_variant = "esp32",
        .device_name = "Improv Demo",
    });

    // Your TODO: handle status updates
    service.set_status_callback([](const improv_ble::StatusUpdate &update) {
        ESP_LOGI(TAG, "State=%u error=%u", static_cast<unsigned>(update.state),
                 static_cast<unsigned>(update.error));
    });

    // Check for existing credentials, otherwise start improv service
    wifi_config_t existing_cfg{};
    if (esp_wifi_get_config(WIFI_IF_STA, &existing_cfg) == ESP_OK &&
        existing_cfg.sta.ssid[0] != '\0') {
        ESP_LOGI(TAG, "Found stored Wi-Fi credentials for SSID \"%s\"",
                 reinterpret_cast<char*>(existing_cfg.sta.ssid));
        ESP_ERROR_CHECK(esp_wifi_connect());
    } else {
        ESP_ERROR_CHECK(service.start());
    }
}
