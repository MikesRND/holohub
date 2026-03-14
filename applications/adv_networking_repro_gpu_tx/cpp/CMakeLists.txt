# SPDX-FileCopyrightText: Copyright (c) 2025-2026, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

cmake_minimum_required(VERSION 3.20)
project(adv_networking_repro_gpu_tx CXX CUDA)

# Dependencies
find_package(holoscan 2.6 REQUIRED CONFIG
             PATHS "/opt/nvidia/holoscan" "/workspace/holoscan-sdk/install")
find_package(CUDAToolkit REQUIRED)
find_package(yaml-cpp REQUIRED)

# Global variables
set(CMAKE_CUDA_ARCHITECTURES "70;80;90")

# Create the executable
add_executable(adv_networking_repro_gpu_tx
  main.cpp
)
target_link_libraries(adv_networking_repro_gpu_tx
  PRIVATE
  holoscan::core
  holoscan::advanced_network
  CUDA::cudart
  yaml-cpp::yaml-cpp
)

# Configure manager support through compile definitions
if(NOT DEFINED ANO_MGR)
    set(ANO_MGR "dpdk" CACHE STRING "Manager type(s) list")
endif()
separate_arguments(ANO_MGR_LIST UNIX_COMMAND ${ANO_MGR})
foreach(MGR IN LISTS ANO_MGR_LIST)
    message(STATUS "Adding manager ${MGR} to the configuration")
    target_compile_definitions(${PROJECT_NAME} PUBLIC "ANO_MGR_${MGR}=1")
endforeach()

# Copy YAML config files
foreach(yaml_file repro.yaml repro-2seg-cpu.yaml repro-1seg-cpu.yaml)
  add_custom_target(adv_networking_repro_gpu_tx_${yaml_file}
    COMMAND ${CMAKE_COMMAND} -E copy "${CMAKE_CURRENT_SOURCE_DIR}/${yaml_file}" ${CMAKE_CURRENT_BINARY_DIR}
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${yaml_file}"
  )
  add_dependencies(adv_networking_repro_gpu_tx adv_networking_repro_gpu_tx_${yaml_file})
endforeach()

# Installation
install(TARGETS adv_networking_repro_gpu_tx
        DESTINATION examples/adv_networking_repro_gpu_tx
        COMPONENT adv_networking_repro_gpu_tx-cpp)

install(
  FILES
    repro.yaml
    repro-2seg-cpu.yaml
    repro-1seg-cpu.yaml
  DESTINATION examples/adv_networking_repro_gpu_tx
  COMPONENT adv_networking_repro_gpu_tx-configs
  PERMISSIONS OWNER_READ OWNER_WRITE
              GROUP_READ GROUP_WRITE
              WORLD_READ WORLD_WRITE
)
