#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <esp_err.h>
#include <esp_event.h>
#include <improv.h>

#include <NimBLECharacteristic.h>
#include <NimBLEServer.h>

class NimBLEAdvertising;
class NimBLEService;

namespace improv_ble {

void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

struct DeviceInfo {
    std::string firmware_name;
    std::string firmware_version;
    std::string hardware_variant;
    std::string device_name;
};

struct StatusUpdate {
    improv::State state;
    improv::Error error;
};

using StatusCallback = std::function<void(const StatusUpdate &)>;

class ImprovBleService {
public:
    friend void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                              void *event_data);

    explicit ImprovBleService(DeviceInfo info = {});
    ImprovBleService(const ImprovBleService &) = delete;
    ImprovBleService &operator=(const ImprovBleService &) = delete;

    esp_err_t start();
    void stop();

    improv::State current_state() const noexcept;
    improv::Error current_error() const noexcept;

    void set_status_callback(StatusCallback cb);

private:
    class RpcCallbacks;
    class ServerCallbacks;

    esp_err_t ensure_server();
    void begin_wifi_connection(const std::string &ssid, const std::string &password);
    void notify_state(improv::State state);
    void notify_error(improv::Error error);
    void send_rpc_response(const std::vector<uint8_t> &payload);
    void update_advertisement_payload();
    void ensure_advertising();
    void handle_improv_command(const std::vector<uint8_t> &data);
    void dispatch_status_update();
    esp_err_t register_event_handlers();
    void handle_wifi_event(esp_event_base_t event_base, int32_t event_id, void *event_data);

    DeviceInfo _device_info{};
    StatusCallback _status_callback;

    NimBLEServer *_server = nullptr;
    NimBLEService *_service = nullptr;
    NimBLECharacteristic *_status_char = nullptr;
    NimBLECharacteristic *_error_char = nullptr;
    NimBLECharacteristic *_rpc_char = nullptr;
    NimBLECharacteristic *_rpc_result_char = nullptr;
    NimBLECharacteristic *_capabilities_char = nullptr;
    NimBLEAdvertising *_advertising = nullptr;

    std::unique_ptr<NimBLECharacteristicCallbacks> _rpc_callbacks;
    std::unique_ptr<NimBLEServerCallbacks> _server_callbacks;

    improv::State _state{improv::STATE_STOPPED};
    improv::Error _error_state{improv::ERROR_NONE};

    // TODO: support CAPABILITY_IDENTIFY?
    uint8_t _capabilities{0};

    bool _advertising_enabled{false};

    esp_event_handler_instance_t _wifi_event_handler = nullptr;
    esp_event_handler_instance_t _ip_event_handler = nullptr;

    std::string _pending_ssid;
    std::string _pending_password;

    uint8_t _status_value{static_cast<uint8_t>(improv::STATE_STOPPED)};
    uint8_t _error_value{static_cast<uint8_t>(improv::ERROR_NONE)};
};

} // namespace improv_ble
