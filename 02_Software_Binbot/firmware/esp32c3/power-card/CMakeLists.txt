cmake_minimum_required(VERSION 3.22)

set(CCACHE_ENABLE OFF CACHE BOOL "Disable ccache" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(power_card)
