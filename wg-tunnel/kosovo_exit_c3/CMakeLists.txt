cmake_minimum_required(VERSION 3.16)
# sdkconfig.base = allgemeine Einstellungen (im Git), sdkconfig.defaults = DEINE geheimen Werte
set(SDKCONFIG_DEFAULTS "sdkconfig.base;sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(kosovo_exit_c3)
