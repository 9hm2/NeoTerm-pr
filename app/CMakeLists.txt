cmake_minimum_required(VERSION 3.4.1)

add_library(neoterm
             SHARED
             src/main/cpp/neoterm.cpp)

target_link_libraries(neoterm)

# uKernel FS engine (real kernel FS drivers in userspace) — cross-compile
# validation target. See src/main/cpp/ukfs/ukfs.cmake. OFF by default so the
# normal app build stays green while the bionic port is iterated; enable the
# cross-compile with:  -DUKFS_BUILD=ON  (gradle: android.defaultConfig.
# externalNativeBuild.cmake.arguments "-DUKFS_BUILD=ON").
option(UKFS_BUILD "Build the uKernel FS engine" ON)
if(UKFS_BUILD)
  include(${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/ukfs/ukfs.cmake)
endif()

# uKernel Wi-Fi framework (chip-agnostic; no driver bundled) — builds the
# libukwifid.so daemon (shim + cfg80211 + usbfs HCD + userver). A chip's vendor
# driver .so is dlopen'd at runtime. See src/main/cpp/ukwifi/ukwifi.cmake and
# wifi/DESIGN.md. Disable with -DUKWIFI_BUILD=OFF.
option(UKWIFI_BUILD "Build the uKernel Wi-Fi framework daemon" ON)
if(UKWIFI_BUILD)
  include(${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/ukwifi/ukwifi.cmake)
endif()

#add_library(nexec
#             SHARED
#             src/main/cpp/exec.c)

