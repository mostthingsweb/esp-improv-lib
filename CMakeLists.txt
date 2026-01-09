cmake_minimum_required(VERSION 3.16)

idf_component_register(
    SRCS
        "src/improv_ble.cpp"
    REQUIRES
        improv
        esp-nimble-cpp
        esp_wifi
        esp_netif
        esp_event
        lwip
    INCLUDE_DIRS
        "include"
)
