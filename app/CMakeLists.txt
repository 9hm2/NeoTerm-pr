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

#add_library(nexec
#             SHARED
#             src/main/cpp/exec.c)

