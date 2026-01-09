#include "improv_ble.h"

#include <cstring>
#include <string>
#include <vector>

#include <esp_event.h>
#include <esp_netif.h>
#include <esp_netif_ip_addr.h>
#include <esp_wifi.h>
#include <lwip/inet.h>

#include <NimBLEAdvertising.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>

#include <esp_log.h>

namespace {
constexpr const char *TAG = "improv_ble";
constexpr uint16_t ImprovServiceDataUuid = 0x4677;

} // namespace

namespace improv_ble {

/// Internal event handler, called by ESP runtime
void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    auto *service = static_cast<ImprovBleService *>(arg);
    if (!service) {
        return;
    }

    // Call the actual event handling code inside the service instance
    service->handle_wifi_event(event_base, event_id, event_data);
}

esp_err_t ImprovBleService::register_event_handlers() {
    bool registered_wifi = false;
    if (_wifi_event_handler == nullptr) {
        esp_err_t err = esp_event_handler_instance_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, this, &_wifi_event_handler);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register Wi-Fi event handler: %s", esp_err_to_name(err));
            return err;
        }
        registered_wifi = true;
    }

    if (_ip_event_handler == nullptr) {
        esp_err_t err = esp_event_handler_instance_register(
            IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, this, &_ip_event_handler);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register IP event handler: %s", esp_err_to_name(err));
            if (registered_wifi) {
                esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                      _wifi_event_handler);
                _wifi_event_handler = nullptr;
            }
            return err;
        }
    }

    return ESP_OK;
}

class ImprovBleService::RpcCallbacks : public NimBLECharacteristicCallbacks {
public:
    explicit RpcCallbacks(ImprovBleService *owner) : _owner(owner) { }

    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &) override {
        const std::string &value = characteristic->getValue();
        if (value.empty()) {
            return;
        }
        std::vector<uint8_t> payload(value.begin(), value.end());
        _owner->handle_improv_command(payload);
    }

private:
    ImprovBleService *_owner;
};

class ImprovBleService::ServerCallbacks : public NimBLEServerCallbacks {
public:
    explicit ServerCallbacks(ImprovBleService *owner) : _owner(owner) { }

    void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override {
        ESP_LOGI(TAG, "Client disconnected");
        _owner->ensure_advertising();
    }

private:
    ImprovBleService *_owner;
};

ImprovBleService::ImprovBleService(DeviceInfo info) : _device_info(std::move(info)) { }

esp_err_t ImprovBleService::start() {
    // Create the server if needed
    esp_err_t err = ensure_server();
    if (err != ESP_OK) {
        return err;
    }

    // Register event handlers, if not already done
    err = register_event_handlers();
    if (err != ESP_OK) {
        return err;
    }

    _advertising_enabled = true;

    notify_error(improv::ERROR_NONE);
    // TODO: when we support authorizer, this needs to be STATE_AWAITING_AUTHORIZATION
    notify_state(improv::STATE_AUTHORIZED);

    update_advertisement_payload();
    ensure_advertising();
    return ESP_OK;
}

void ImprovBleService::stop() {
    _advertising_enabled = false;
    if (_advertising && _advertising->isAdvertising()) {
        _advertising->stop();
        ESP_LOGI(TAG, "Advertising stopped");
    }

    notify_state(improv::STATE_STOPPED);
    notify_error(improv::ERROR_NONE);
}

void ImprovBleService::notify_state(improv::State state) {
    if (_state == state) {
        dispatch_status_update();
        return;
    }

    //ESP_LOGE(TAG, "state %d", state);

    _state = state;
    _status_value = static_cast<uint8_t>(_state);

    if (_status_char) {
        _status_char->setValue(&_status_value, 1);
        _status_char->notify();
    }

    dispatch_status_update();
}

void ImprovBleService::notify_error(improv::Error error) {
    if (_error_state == error) {
        dispatch_status_update();
        return;
    }

    //ESP_LOGE(TAG, "notify error %d", error);

    _error_state = error;
    _error_value = static_cast<uint8_t>(_error_state);

    if (_error_char) {
        _error_char->setValue(&_error_value, 1);
        _error_char->notify();
    }
    dispatch_status_update();
}

void ImprovBleService::send_rpc_response(const std::vector<uint8_t> &payload) {
    if (!_rpc_result_char) {
        return;
    }

    _rpc_result_char->setValue(payload.data(), payload.size());
    if (!payload.empty()) {
        _rpc_result_char->notify();
    }
}

improv::State ImprovBleService::current_state() const noexcept {
    return _state;
}

improv::Error ImprovBleService::current_error() const noexcept {
    return _error_state;
}

void ImprovBleService::set_status_callback(StatusCallback cb) {
    _status_callback = std::move(cb);
    dispatch_status_update();
}

esp_err_t ImprovBleService::ensure_server() {
    if (_server) {
        return ESP_OK;
    }

    const std::string device_name =
        _device_info.device_name.empty() ? std::string{"ImprovDevice"} : _device_info.device_name;

    NimBLEDevice::init(device_name);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    NimBLEDevice::setSecurityAuth(false, false, false);

    _server = NimBLEDevice::createServer();
    if (!_server) {
        ESP_LOGE(TAG, "Failed to create NimBLE server");
        return ESP_FAIL;
    }

    _server_callbacks = std::make_unique<ServerCallbacks>(this);
    _server->setCallbacks(_server_callbacks.get());

    _service = _server->createService(improv::SERVICE_UUID);
    if (!_service) {
        ESP_LOGE(TAG, "Failed to create NimBLE service");
        return ESP_FAIL;
    }

    _status_char = _service->createCharacteristic(improv::STATUS_UUID,
                                                  NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    _error_char = _service->createCharacteristic(improv::ERROR_UUID,
                                                 NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    _rpc_char = _service->createCharacteristic(improv::RPC_COMMAND_UUID,
                                               NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
    _rpc_result_char = _service->createCharacteristic(
        improv::RPC_RESULT_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    _capabilities_char =
        _service->createCharacteristic(improv::CAPABILITIES_UUID, NIMBLE_PROPERTY::READ);

    if (!_status_char || !_error_char || !_rpc_char || !_rpc_result_char || !_capabilities_char) {
        ESP_LOGE(TAG, "Failed to create one or more characteristics");
        return ESP_FAIL;
    }

    _rpc_callbacks = std::make_unique<RpcCallbacks>(this);
    _rpc_char->setCallbacks(_rpc_callbacks.get());

    _status_char->setValue(&_status_value, 1);
    _error_char->setValue(&_error_value, 1);
    _rpc_result_char->setValue(std::string{});
    _capabilities_char->setValue(&_capabilities, 1);

    _service->start();

    _advertising = NimBLEDevice::getAdvertising();
    _advertising->addServiceUUID(_service->getUUID());

    return ESP_OK;
}

void ImprovBleService::begin_wifi_connection(const std::string &ssid, const std::string &password) {
    _pending_ssid = ssid;
    _pending_password = password;

    notify_state(improv::STATE_PROVISIONING);
    notify_error(improv::ERROR_NONE);

    wifi_config_t config{};
    std::memset(&config, 0, sizeof(config));
    std::strncpy(reinterpret_cast<char *>(config.sta.ssid), ssid.c_str(),
                 sizeof(config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char *>(config.sta.password), password.c_str(),
                 sizeof(config.sta.password) - 1);
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set Wi-Fi mode: %s", esp_err_to_name(err));
    }

    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set Wi-Fi config: %s", esp_err_to_name(err));
        notify_error(improv::ERROR_UNABLE_TO_CONNECT);
        notify_state(improv::STATE_AUTHORIZED);
        return;
    }

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        notify_error(improv::ERROR_UNABLE_TO_CONNECT);
        notify_state(improv::STATE_AUTHORIZED);
    }
}

void ImprovBleService::update_advertisement_payload() {
    if (!_advertising_enabled || _advertising == nullptr || _service == nullptr) {
        return;
    }

    NimBLEAdvertisementData adv_data;
    adv_data.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
    adv_data.addServiceUUID(_service->getUUID());

    std::string service_data;
    service_data.reserve(6);
    service_data.push_back(static_cast<char>(_status_value));
    service_data.push_back(static_cast<char>(_capabilities));
    service_data.append(4, '\0');
    adv_data.setServiceData(NimBLEUUID{ImprovServiceDataUuid}, service_data);

    _advertising->setAdvertisementData(adv_data);

    NimBLEAdvertisementData scan_data;
    if (!_device_info.device_name.empty()) {
        scan_data.setName(_device_info.device_name);
    }
    _advertising->setScanResponseData(scan_data);
}

void ImprovBleService::ensure_advertising() {
    if (!_advertising_enabled || _advertising == nullptr) {
        return;
    }
    if (!_advertising->isAdvertising()) {
        _advertising->start();
        ESP_LOGI(TAG, "Advertising started");
    }
}

void ImprovBleService::dispatch_status_update() {
    if (!_status_callback) {
        return;
    }
    _status_callback(StatusUpdate{_state, _error_state});
}

void ImprovBleService::handle_improv_command(const std::vector<uint8_t> &data) {
    if (data.size() < 3) {
        ESP_LOGW(TAG, "Improv command too short");
        notify_error(improv::ERROR_INVALID_RPC);
        return;
    }

    improv::ImprovCommand command = improv::parse_improv_data(data);
    switch (command.command) {
    case improv::BAD_CHECKSUM:
        ESP_LOGW(TAG, "Improv checksum failure");
        notify_error(improv::ERROR_INVALID_RPC);
        break;
    case improv::WIFI_SETTINGS:
        ESP_LOGI(TAG, "Improv Wi-Fi credentials received (ssid=%s)", command.ssid.c_str());
        begin_wifi_connection(command.ssid, command.password);
        break;
    case improv::IDENTIFY:
        // TODO
        ESP_LOGI(TAG, "Identify request received - not implemented");
        break;
    case improv::GET_DEVICE_INFO:
        notify_error(improv::ERROR_NONE);
        send_rpc_response(improv::build_rpc_response(
            improv::GET_DEVICE_INFO, {_device_info.firmware_name, _device_info.firmware_version,
                                      _device_info.hardware_variant, _device_info.device_name}));
        break;
    default:
        ESP_LOGW(TAG, "Unknown Improv command: 0x%02X", static_cast<int>(command.command));
        notify_error(improv::ERROR_UNKNOWN_RPC);
        break;
    }
}

void ImprovBleService::handle_wifi_event(esp_event_base_t event_base, int32_t event_id,
                                         void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (_state == improv::STATE_PROVISIONING) {
            esp_wifi_connect();
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = static_cast<ip_event_got_ip_t *>(event_data);
        char ip_str[INET_ADDRSTRLEN] = "";
        if (event) {
            esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
        }

        notify_error(improv::ERROR_NONE);
        notify_state(improv::STATE_PROVISIONED);

        std::vector<std::string> urls;
        if (std::strlen(ip_str) > 0) {
            // TODO: allow customizing URLs, like esphome does
            // https://github.com/esphome/esphome/blob/dev/esphome/components/esp32_improv/esp32_improv_component.cpp#L412
            urls.emplace_back(std::string("http://") + ip_str);
        }

        send_rpc_response(improv::build_rpc_response(improv::WIFI_SETTINGS, urls));

        if (!_pending_ssid.empty() || !_pending_password.empty()) {
            wifi_config_t cfg{};
            std::memset(&cfg, 0, sizeof(cfg));
            std::strncpy(reinterpret_cast<char *>(cfg.sta.ssid), _pending_ssid.c_str(),
                         sizeof(cfg.sta.ssid) - 1);
            std::strncpy(reinterpret_cast<char *>(cfg.sta.password), _pending_password.c_str(),
                         sizeof(cfg.sta.password) - 1);
            cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
            ESP_LOGI(TAG, "SAVING WIFI CONFIG");
            esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "Failed to persist Wi-Fi config: %s", esp_err_to_name(err));
            }
            _pending_ssid.clear();
            _pending_password.clear();
        }

        _advertising_enabled = false;
        if (_advertising && _advertising->isAdvertising()) {
            _advertising->stop();
        }
    }
}

} // namespace improv_ble
