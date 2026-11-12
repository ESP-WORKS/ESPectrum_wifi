cmake_minimum_required(VERSION 3.16.0)

list(APPEND EXTRA_COMPONENT_DIRS
    components/fabgl
    components/pwm_audio
    components/bluepad32
    components/btstack
    components/cmd_nvs_4_4
    components/cmd_system_4_4
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ESPectrum2)
