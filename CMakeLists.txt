# ===========================================================================
#  CMakeLists.txt  --  HeteroSTA3D 3D-STA driver
# ===========================================================================
#  Stub build (default, no GPU/license needed):
#      cmake -S . -B build -DH3D_USE_STUB=ON
#      cmake --build build
#
#  Real build (link the vendor library):
#      cmake -S . -B build -DH3D_USE_STUB=OFF \
#            -DH3D_DIR=/path/to/heterosta3d-release \
#            -DCUDA_DIR=/usr/local/cuda
#      cmake --build build
# ===========================================================================
cmake_minimum_required(VERSION 3.16)
project(heterosta3d_driver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(H3D_USE_STUB "Build against the bundled CPU stub instead of the real lib" ON)
set(H3D_DIR  "/opt/heterosta3d" CACHE PATH "Unpacked HeteroSTA3D release")
set(CUDA_DIR "/usr/local/cuda"  CACHE PATH "CUDA toolkit root")

add_executable(sta_driver src/sta_driver.cpp)
target_include_directories(sta_driver PRIVATE src)

if(H3D_USE_STUB)
  message(STATUS "HeteroSTA3D: STUB build (CPU, no GPU/license)")
  target_sources(sta_driver PRIVATE stub/heterosta3d_stub.cpp)
  target_include_directories(sta_driver PRIVATE third_party/heterosta3d/include)
  target_compile_definitions(sta_driver PRIVATE H3D_USE_STUB)
else()
  message(STATUS "HeteroSTA3D: REAL build against ${H3D_DIR}")
  target_include_directories(sta_driver PRIVATE "${H3D_DIR}/include")
  target_link_directories(sta_driver PRIVATE "${H3D_DIR}/lib" "${CUDA_DIR}/lib64")
  target_link_libraries(sta_driver PRIVATE heterosta3d cudart)
  set_target_properties(sta_driver PROPERTIES
    BUILD_RPATH "${H3D_DIR}/lib")
endif()

if(MSVC)
  target_compile_options(sta_driver PRIVATE /W3)
else()
  target_compile_options(sta_driver PRIVATE -Wall -Wextra)
endif()
