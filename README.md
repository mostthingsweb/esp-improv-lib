# esp-improv-lib

An ESP-IDF component that implements the [Improv Wi-Fi protocol](https://www.improv-wifi.com/). Includes an example app.

Largely inspired by [the implementation in esphome](https://github.com/esphome/esphome/tree/dev/esphome/components/esp32_improv).

## TODOs
Only basic support for the protocol is implemented. A bunch of stuff is missing, see [https://www.improv-wifi.com/ble/](https://www.improv-wifi.com/ble/).

1. Support authorizer
2. Support custom URLs for provision response message
3. Support 'identify' capability
4. Support the rest of the commands in the protocol.

## Layout
- `CMakeLists.txt`, `idf_component.yml`, `src/`, `include/`: the library component (`esp-improv-lib`)
- `examples/basic`: ESP-IDF app demonstrating how to consume the library locally

## Build the example
The example is a self-contained ESP-IDF app. It depends on this component via `examples/basic/main/idf_component.yml`.

1. From the repository root run `idf.py -C examples/basic reconfigure build`.
2. Flash and monitor with `idf.py -C examples/basic -p PORT flash monitor`.
