cmake_minimum_required(VERSION 3.16)

# Componentes compartidos con el HUD (esp/Firmware). El framing del Ruptela y el
# de NMEA se compilan desde un solo lugar para los dos proyectos.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(starlink_pilot)
