cmake_minimum_required(VERSION 3.10)
include(CheckCSourceRuns)
cmake_policy(SET CMP0054 NEW)
#Allow project version
cmake_policy(SET CMP0048 NEW)
project(Agora VERSION 1.2.2)

if(CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 7.0)
  #Flags is enabled by default for c++17
  set(GCC_COVERAGE_COMPILE_FLAGS "-faligned-new")
endif()

set(SOURCE_DIR ".")
configure_file(${CMAKE_SOURCE_DIR}/src/common/version_config.h.in ${CMAKE_SOURCE_DIR}/src/common/version_config.h)

option(FORCE_BUILD_PATH "Hardcode the build directory path to be './build/'" ON)
if(FORCE_BUILD_PATH)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/build)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/build)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/build)
endif()

# New versions of GCC enable position-independent executables by default. We
# don't want to compile FlexRAN libraries with -fPIC because it reduces
# performance by around 40%.  Don’t produce a dynamically linked position independent executable. 
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -no-pie")

set(CMAKE_CXX_STANDARD 17)

if(${CMAKE_C_COMPILER_ID} STREQUAL "GNU")
  message(STATUS "Using GNU compiler, compiler ID ${CMAKE_C_COMPILER_ID}")
  set(CMAKE_C_FLAGS "-std=gnu11 -Wall -g -march=native -m64")
  set(CMAKE_CXX_FLAGS "-std=c++17 -Wall -g -march=native -m64")
  #Set MKL Threading model here if needed (lmkl_intel_lp64 + lmkl_tbb_thread | lmkl_sequential | lmkl_intel_thread)
  #Set -lmkl_rt for dynamic setting of mkl_set_interface_layer / mkl_set_threading_layer
  if (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.2)
    message(STATUS "GCC version: ${CMAKE_CXX_COMPILER_VERSION} (versions newer than 9.2 are suggested for using AVX512 optimizations)")
  endif()
elseif(${CMAKE_C_COMPILER_ID} STREQUAL "Intel")
  message(STATUS "Using Intel compiler, compiler ID ${CMAKE_C_COMPILER_ID}")
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/intel-compile-options.cmake)
  set(CMAKE_CXX_FLAGS "-std=c++17 -Wall -g -march=native -mkl=sequential")
elseif("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang" OR "${CMAKE_CXX_COMPILER_ID}" STREQUAL "AppleClang")
  message(STATUS "Using Clang compiler, compiler ID ${CMAKE_C_COMPILER_ID}")
endif()

#Set to ON For clang tidy
set(CMAKE_EXPORT_COMPILE_COMMANDS OFF)

option(DEBUG "Enable debugging" OFF)

if(CMAKE_BUILD_TYPE MATCHES Debug)
  message(STATUS "Enabling debug mode due to build type")
  set(DEBUG ON)
endif()

if(NOT DEBUG)
  message(STATUS "Debugging is disabled")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -Ofast -DNDEBUG")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -Ofast -DNDEBUG")
else()
  message(STATUS "Verbose Makefile")
  set( CMAKE_VERBOSE_MAKEFILE on )
  message(STATUS "Debugging is enabled. Performance will be low")
  set(CMAKE_BUILD_TYPE Debug)
  set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Wshadow -ggdb3 -fstack-protector-all -fsanitize=undefined -fsanitize=address -fsanitize=null")
  #-fsanitize=thread- fstack-protector-all -fsanitize=address -fstack-check
  #-fsanitize=address -fsanitize=pointer-compare
  #-fsanitize=address -fsanitize=pointer-subtract
  #-fsanitize=leak -fsanitize=undefined -fsanitize=null
endif()

message(STATUS "CMAKE_CXX_FLAGS: ${CMAKE_CXX_FLAGS}")
message(STATUS "CURRENT DIRECTORY: ${CMAKE_CURRENT_SOURCE_DIR}")
message(STATUS "CMAKE_CURRENT_SOURCE_DIR: ${CMAKE_CURRENT_SOURCE_DIR}")
add_definitions(-DPROJECT_DIRECTORY=${SOURCE_DIR})

set(FLEXRAN_FEC_SDK_DIR /opt/FlexRAN-FEC-SDK-19-04/sdk)

# Determine if the current machine supports AVX-512
CHECK_C_SOURCE_RUNS("int main() { asm volatile(\"vmovdqu64 %zmm0, %zmm1\"); return 0; }" ISA_AVX512)
if (ISA_AVX512)
  message(STATUS "Processor supports AVX-512")
  add_definitions(-DISA_AVX512)
  # Leave USE_AVX2_ENCODER unmodified
else()
  message(STATUS "Processor does not support AVX-512")
  set(USE_AVX2_ENCODER True)
endif()

if(USE_AVX2_ENCODER)
  message(STATUS "Using Agora's (i.e., not FlexRAN's) AVX2 encoder")
  set(FLEXRAN_FEC_LIB_DIR ${FLEXRAN_FEC_SDK_DIR}/build-avx2-icc)
  add_definitions(-DUSE_AVX2_ENCODER)
else()
  message(STATUS "Using FlexRAN's (i.e., not Agora's) AVX512 encoder")
  set(FLEXRAN_FEC_LIB_DIR ${FLEXRAN_FEC_SDK_DIR}/build-avx512-icc)
endif()

#Settable values
message(STATUS "\n-- ----- Configuration values -----")
message(STATUS "DEBUG:            ${DEBUG}")
set(RADIO_TYPE SIMULATION CACHE STRING "RADIO_TYPE defaulting to 'SIMULATION', valid types are SIMULATION / SOAPY_IRIS / PURE_UHD / DPDK")
message(STATUS "RADIO_TYPE:       ${RADIO_TYPE}")
set(LOG_LEVEL "info" CACHE STRING "Console logging level (none/error/warn/info/frame/subframe/trace)") 
message(STATUS "LOG_LEVEL:        ${LOG_LEVEL}")
set(USE_SPDLOG True CACHE BOOL "USE_SPDLOG set to true to use the spdlog library for logging support")
message(STATUS "USE_SPDLOG:       ${USE_SPDLOG}")
set(ENABLE_MAC False CACHE BOOL "ENABLE_MAC set to true to enable mac support")
message(STATUS "ENABLE_MAC:       ${ENABLE_MAC}")
set(ENABLE_CSV_LOG False CACHE BOOL "ENABLE_CSV_LOG set to enable csv log output")
message(STATUS "ENABLE_CSV_LOG:   ${ENABLE_CSV_LOG}")
set(ENABLE_MAT_LOG False CACHE BOOL "ENABLE_MAT_LOG set to enable matrix log output")
message(STATUS "ENABLE_MAT_LOG:   ${ENABLE_MAT_LOG}")
set(USE_AVX2_ENCODER False CACHE BOOL "Use Agora's AVX2 encoder instead of FlexRAN's AVX512 encoder")
message(STATUS "USE_AVX2_ENCODER: ${USE_AVX2_ENCODER}")
set(ENABLE_HDF5 False CACHE BOOL "ENABLE_HDF5 defaulting to 'False'")
message(STATUS "ENABLE_HDF5:      ${ENABLE_HDF5}")
set(TIME_EXCLUSIVE False CACHE BOOL "TIME_EXCLUSIVE defaulting to 'False'")
message(STATUS "TIME_EXCLUSIVE:   ${TIME_EXCLUSIVE}")
set(LDPC_TYPE FlexRAN CACHE STRING "LDPC_TYPE defaulting to 'FlexRAN', valid types are FlexRAN / ACC100")
message(STATUS "LDPC_TYPE:        ${LDPC_TYPE}")
set(GENDATA_ENCODE FlexRAN CACHE STRING "GENDATA_ENCODE defaulting to 'FlexRAN', valid types are FlexRAN / ACC100")
message(STATUS "GENDATA_ENCODE:   ${GENDATA_ENCODE}")
set(LDPC_ENQ_BULK False CACHE BOOL "LDPC_ENQ_BULK defaulting to 'False'")
message(STATUS "LDPC_ENQ_BULK:    ${LDPC_ENQ_BULK}")
set(MAT_OP_TYPE ARMA_VEC CACHE STRING "MAT_OP_TYPE defaulting to 'ARMA_VEC', valid types are ARMA_CUBE / ARMA_VEC / AVX512, works under \"small_mimo_acc\"")
message(STATUS "MAT_OP_TYPE:      ${MAT_OP_TYPE}")
set(SINGLE_THREAD False CACHE BOOL "ENABLE_SINGLE_THREAD defaulting to 'False'")
message(STATUS "SINGLE_THREAD:    ${SINGLE_THREAD}")
message(STATUS "--------------------------------\n--")

if(RADIO_TYPE STREQUAL SOAPY_IRIS)
  message(STATUS "Enabled SOAPY Iris radio type")
  add_definitions(-DUSE_ARGOS)
elseif(RADIO_TYPE STREQUAL SOAPY_UHD)
  message(STATUS "Enabled SOAPY UHD radio type")
  add_definitions(-DUSE_UHD)
elseif(RADIO_TYPE STREQUAL PURE_UHD)
  message(STATUS "Enabled Pure UHD radio type")
  add_definitions(-DUSE_PURE_UHD)
elseif(RADIO_TYPE STREQUAL DPDK)
  message(STATUS "Enabled DPDK radio type")
  add_definitions(-DUSE_DPDK)
else()
  message(STATUS "Enabled SIMULATION radio type")
endif()

if(LDPC_TYPE STREQUAL ACC100)
  message(STATUS "Enabled HW Intel ACC100 LDPC Decoding (DPDK is mandatory)")
  add_definitions(-DUSE_ACC100)
else()
  message(STATUS "Enabled SW Intel FlexRAN LDPC Decoding")
endif()

if(GENDATA_ENCODE STREQUAL ACC100)
  message(STATUS "Enabled HW Intel ACC100 LDPC Encoding (DPDK is mandatory)")
  add_definitions(-DUSE_ACC100_ENCODE)
else()
  message(STATUS "Enabled SW Intel FlexRAN LDPC Encoding")
endif()

if(${LDPC_ENQ_BULK})
  message(STATUS "LDPC: Bulk Enqueue Mode")
  add_definitions(-DENQUEUE_BULK)
else()
  message(STATUS "LDPC: Sequential Enqueue Mode")
endif()

#External Libraries / Depends
find_library(NUMA_LIBRARIES numa REQUIRED)
message(VERBOSE "  Numa: Libraries ${NUMA_LIBRARIES}")

# Intel MKL (threaded Intel10_64lp: Non-threaded Intel10_64lp_seq)
set(BLA_VENDOR Intel10_64lp_seq)
find_package(BLAS REQUIRED)
include_directories(${BLAS_INCLUDE_DIR})

# DPDK
if((RADIO_TYPE STREQUAL DPDK) AND (LDPC_TYPE STREQUAL ACC100))
  message(WARNING "Both radio and LDPC are using DPDK")
  message(FATAL_ERROR "Current implementation does not support dual-usage of DPDK (radio & ACC100)")
endif()
if((RADIO_TYPE STREQUAL DPDK) OR (LDPC_TYPE STREQUAL ACC100) OR (GENDATA_ENCODE STREQUAL ACC100))
  find_library(DPDK_LIB dpdk)
  if(NOT DPDK_LIB)
    #If cmake doesn't find DPDK than have pkg-config attempt the same
    find_package(PkgConfig REQUIRED)
    pkg_search_module(DPDK REQUIRED libdpdk>=21.11.1)
    message(STATUS "  DPDK version ${DPDK_VERSION} is enabled for Agora")
    set(DPDK_LIBRARIES ${DPDK_LINK_LIBRARIES})
  endif()

  if(NOT DPDK_LIBRARIES)
    message(FATAL_ERROR "DPDK library not found")
  endif()

  if (DPDK_INCLUDE_DIRS)
    message(VERBOSE "  DPDK include directory = ${DPDK_INCLUDE_DIRS}  Libraries: ${DPDK_LIBRARIES}")
  else()
    message(FATAL_ERROR "DPDK include directory not found")
  endif()
  include_directories(${DPDK_INCLUDE_DIRS})

  # add_definitions(-DUSE_DPDK)
  if (ISA_AVX512)
    message(STATUS "Enabling AVX-512 memcpy implementation for dpdk")
    add_definitions(-DRTE_MEMCPY_AVX512)
  endif()
endif()

#Armadillo
find_package(Armadillo "11.0.0" REQUIRED)
message(VERBOSE "  Armadillo: Includes ${ARMADILLO_INCLUDE_DIR} Libraries: ${ARMADILLO_LIBRARIES}")
add_definitions(-DARMA_DONT_USE_WRAPPER -DARMA_DONT_USE_FORTRAN_HIDDEN_ARGS)
include_directories(${ARMADILLO_INCLUDE_DIR})

#HDF5 (optional)
if (${ENABLE_HDF5})
  add_definitions(-DENABLE_HDF5)
  find_package(HDF5 1.10 REQUIRED COMPONENTS CXX)
  if (NOT HDF5_FOUND)
      message(FATAL_ERROR "HDF5 development files not found")
      return()
  endif()
  message(VERBOSE "  HDF5 Includes: ${HDF5_INCLUDE_DIRS} Libraries: ${HDF5_LIBRARIES}")
  include_directories(${HDF5_INCLUDE_DIRS})
endif()

#Time-exclusive Report, disable all phy stat except BER and BLER for verification
if (${TIME_EXCLUSIVE})
  add_definitions(-DTIME_EXCLUSIVE)
  message("-- Time-exlusive: Report only timing but not other characteristics")
endif()

#AVX512 Matrix Operation (used in Dobeamweights.cc and DoDemul.cc)
if(MAT_OP_TYPE STREQUAL ARMA_CUBE)
  add_definitions(-DARMA_CUBE_MATOP)
  message("-- MAT_OP_TYPE: Enable Armadillo cube matrix operation")
elseif(MAT_OP_TYPE STREQUAL ARMA_VEC)
  add_definitions(-DARMA_VEC_MATOP)
  message("-- MAT_OP_TYPE: Enable Armadillo vector matrix operation")
elseif(MAT_OP_TYPE STREQUAL AVX512)
  if (NOT ISA_AVX512)
    message(FATAL_ERROR "AVX512_MATOP is enabled but the current machine does not support AVX512")
  endif()
  add_definitions(-DAVX512_MATOP)
  message("-- MAT_OP_TYPE: Enable AVX512 matrix operation")
else()
  message("-- MAT_OP_TYPE: Disable matrix operation, fall back to Armadillo")
endif()

#Single thread
if (${SINGLE_THREAD})
  add_definitions(-DSINGLE_THREAD)
  message("-- SINGLE_THREAD: Forbit worker thread, only use main thread for processing")
else()
  message("-- SINGLE_THREAD: Enable worker thread (use multi-thread model)")
endif()

#Python
find_package(PythonLibs REQUIRED)
set(PYTHON_LIB ${PYTHON_LIBRARIES})
include_directories(${PYTHON_INCLUDE_DIRS})
message(VERBOSE "  PythonLibs Includes: ${PYTHON_INCLUDE_DIRS} Libraries: ${PYTHON_LIBRARIES}")

#Soapy (optional based on radio type)
if(RADIO_TYPE STREQUAL SOAPY_IRIS)
  find_package(SoapySDR "0.8.0" CONFIG)
  if(NOT SoapySDR_FOUND)
    message(FATAL_ERROR "SoapySDR development files not found")
  endif()
  set(SOAPY_LIB ${SoapySDR_LIBRARIES})
  include_directories(${SOAPY_INCLUDE_DIRS})
  message(VERBOSE "  SoapySDR Includes: ${SoapySDR_INCLUDE_DIRS} Libraries: ${SoapySDR_LIBRARIES}")

  set(RADIO_SOURCES
      src/radio/radio.cc
      src/radio/radio_soapysdr.cc
      src/radio/radio_data_plane.cc
      src/radio/radio_socket.cc
      src/radio/radio_data_plane_soapy.cc
      src/radio/radio_data_plane_socket.cc
      src/radio/radio_set/radio_set.cc
      )

  set(RADIO_SOURCES_AGORA
      src/agora/txrx/packet_txrx_radio.cc
      src/agora/txrx/workers/txrx_worker_hw.cc
      src/radio/radio_set/radio_set_bs.cc
      )

  set(RADIO_SOURCES_CLIENT
      src/radio/radio_set/radio_set_ue.cc
      src/client/txrx/packet_txrx_client_radio.cc
      src/client/txrx/workers/txrx_worker_client_hw.cc
      )
endif()

#SPDLOG
if(USE_SPDLOG)
  if(LOG_LEVEL STREQUAL "none")
    add_definitions(-DSPDLOG_LEVEL=spdlog::level::off)
    add_definitions(-DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_OFF)
  elseif(LOG_LEVEL STREQUAL "error")
    add_definitions(-DSPDLOG_LEVEL=spdlog::level::err)
    add_definitions(-DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_ERROR)
  elseif(LOG_LEVEL STREQUAL "warn")
    add_definitions(-DSPDLOG_LEVEL=spdlog::level::warn)
    add_definitions(-DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_WARN)
  elseif(LOG_LEVEL STREQUAL "info")
    add_definitions(-DSPDLOG_LEVEL=spdlog::level::info)
    add_definitions(-DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO)
  elseif(LOG_LEVEL STREQUAL "frame")
    add_definitions(-DSPDLOG_LEVEL=spdlog::level::debug)
    add_definitions(-DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)
  elseif(LOG_LEVEL STREQUAL "subframe")
    add_definitions(-DSPDLOG_LEVEL=spdlog::level::debug)
    add_definitions(-DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)
  elseif(LOG_LEVEL STREQUAL "trace")
    add_definitions(-DSPDLOG_LEVEL=spdlog::level::trace)
    add_definitions(-DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)
  endif()

  message(STATUS "Using spdlog async logger as the default logger")
  add_definitions(-DUSE_SPDLOG=true)
  add_definitions(-DSPDLOG_COMPILED_LIB)
  add_subdirectory(third_party/spdlog)
  set(COMMON_LIBS spdlog::spdlog)
else ()
  message(STATUS "Using agora raw logger")
endif()

if(USE_SPDLOG AND ENABLE_CSV_LOG)
  add_definitions(-DENABLE_CSV_LOG=true)
  message(STATUS "Enabled Csv Logger")
  if(ENABLE_MAT_LOG)
    add_definitions(-DENABLE_MAT_LOG=true)
    message(STATUS "Enabled Mat Logger")
  else()
    message(STATUS "Disabled Mat Logger")
  endif()
else()
  message(STATUS "Disabled Csv/Mat Logger")
endif()

# Console logging level
if(LOG_LEVEL STREQUAL "none")
  message(STATUS "Logging level = none.")
  add_definitions(-DAGORA_LOG_LEVEL=0)
elseif(LOG_LEVEL STREQUAL "error")
  message(STATUS "Logging level = error.")
  add_definitions(-DAGORA_LOG_LEVEL=1)
elseif(LOG_LEVEL STREQUAL "warn")
  message(STATUS "Logging level = warn.")
  add_definitions(-DAGORA_LOG_LEVEL=2)
elseif(LOG_LEVEL STREQUAL "info")
  message(STATUS "Logging level = info.")
  add_definitions(-DAGORA_LOG_LEVEL=3)
elseif(LOG_LEVEL STREQUAL "frame")
  message(STATUS "Logging level = frame. Warning: Performance will be low.")
  add_definitions(-DAGORA_LOG_LEVEL=4)
elseif(LOG_LEVEL STREQUAL "subframe")
  message(STATUS "Logging level = subframe. Warning: Performance will be low.")
  add_definitions(-DAGORA_LOG_LEVEL=5)
elseif(LOG_LEVEL STREQUAL "trace")
  message(STATUS "Logging level = trace. Warning: Performance will be low.")
  add_definitions(-DAGORA_LOG_LEVEL=6)
else()
  message(STATUS "No logging level specified. Using warning level.")
  add_definitions(-DAGORA_LOG_LEVEL=2)
endif()

#Gflags
find_package(gflags REQUIRED)
message(VERBOSE "  gflags: Includes ${GFLAGS_INCLUDE_DIR} Libraries: ${GFLAGS_LIBRARIES}")
include_directories(${GFLAGS_INCLUDE_DIR})

#Decoder
if(LDPC_TYPE STREQUAL ACC100)
  set(DECODER_SOURCES_AGORA src/agora/dodecode_acc.cc)
elseif(LDPC_TYPE STREQUAL FlexRAN)
  set(DECODER_SOURCES_AGORA src/agora/dodecode.cc)
endif()

#Flexran
include_directories(
  ${FLEXRAN_FEC_LIB_DIR}/install/lib_ldpc_decoder_5gnr
  ${FLEXRAN_FEC_LIB_DIR}/install/lib_ldpc_encoder_5gnr
  ${FLEXRAN_FEC_LIB_DIR}/install/lib_common
  ${SOURCE_DIR}/src/encoder)

set(FLEXRAN_LDPC_LIBS
  ${FLEXRAN_FEC_LIB_DIR}/install/lib_ldpc_encoder_5gnr/libldpc_encoder_5gnr.a
  ${FLEXRAN_FEC_LIB_DIR}/install/lib_ldpc_decoder_5gnr/libldpc_decoder_5gnr.a
  ${FLEXRAN_FEC_LIB_DIR}/install/lib_common/libcommon.a)

# Unit tests
enable_testing()
find_package(GTest REQUIRED)
message(VERBOSE "  GTest: Includes ${GTEST_INCLUDE_DIR} Libraries ${GTEST_LIBRARIES}")
set(GTEST_LIBRARIES ${GTEST_LIBRARIES} -lpthread)

#third party libs included in our repo
include_directories(third_party third_party/spdlog/include third_party/nlohmann/single_include)

add_definitions(-DTHREADED_INIT)

if(RADIO_TYPE STREQUAL PURE_UHD)
  message(STATUS "Enabled PURE_UHD radio")
  add_definitions(-DUSE_UHD)
  # Adding dependency of UHD and boost external libraries
  find_package(UHD "3.15.0" REQUIRED)
  message(STATUS "Using UHD Version: ${UHD_VERSION}")

  ########################################################################
  # PURE UHD dependencies
  ########################################################################
  # Adding dependency of Boost to souder
  set(BOOST_REQUIRED_COMPONENTS
      program_options
      system
      thread)

  if(MSVC)
      set(BOOST_ALL_DYN_LINK "${BOOST_ALL_DYN_LINK}" CACHE BOOL "boost enable dynamic linking")
      if(BOOST_ALL_DYN_LINK)
          add_definitions(-DBOOST_ALL_DYN_LINK) #setup boost auto-linking in msvc
      else(BOOST_ALL_DYN_LINK)
          set(BOOST_REQUIRED_COMPONENTS) #empty components list for static link
      endif(BOOST_ALL_DYN_LINK)
  endif(MSVC)
  find_package(Boost "1.65" REQUIRED ${BOOST_REQUIRED_COMPONENTS})
  message(STATUS "Using Boost Version: ${Boost_VERSION}")

  set(RADIO_SOURCES
      src/radio/radio_set/radio_set_uhd.cc
      src/radio/radio_uhdsdr.cc
      src/radio/radio.cc
      src/radio/radio_set/radio_set.cc
      )
  set(RADIO_SOURCES_AGORA
      src/agora/txrx/workers/txrx_worker_usrp.cc
      src/agora/txrx/packet_txrx_radio.cc
      src/agora/txrx/workers/txrx_worker_hw.cc
      # src/radio/radio_set/radio_set_bs.cc

      )

  set(RADIO_SOURCES_CLIENT
      src/client/txrx/workers/txrx_worker_client_uhd.cc
      src/client/txrx/packet_txrx_client_radio.cc
      src/client/txrx/workers/txrx_worker_client_hw.cc
      src/radio/radio_set/radio_set_ue.cc
      )
  message(STATUS "pure_uhd_lib library initialized")
endif()

# MAC
if(${ENABLE_MAC})
  add_definitions(-DENABLE_MAC)
endif()

set(MAC_CLIENT_SOURCES
  src/mac/mac_sender.cc
  src/mac/mac_receiver.cc
  src/mac/video_receiver.cc
  src/mac/file_receiver.cc
  src/mac/mac_client.cc)

set(MAC_BS_SOURCES
  src/mac/mac_sender.cc
  src/mac/mac_receiver.cc
  src/mac/video_receiver.cc
  src/mac/file_receiver.cc
  src/mac/mac_basestation.cc)

include_directories(
  src/common/
  src/common/ipc
  src/common/loggers
  src/recorder
  src/mac/
  src/radio
  src/radio/radio_set
  src/agora/
  src/agora/txrx
  src/agora/txrx/workers
  src/client
  src/client/txrx
  src/client/txrx/workers
  src/data_generator)

set(RECORDER_SOURCES
  src/recorder/recorder_thread.cc
  src/recorder/recorder_worker.cc
  src/recorder/recorder_worker_multifile.cc)

if (${ENABLE_HDF5})
  set(RECORDER_SOURCES
      ${RECORDER_SOURCES}
      src/recorder/hdf5_lib.cc
      src/recorder/recorder_worker_hdf5.cc)
endif()
add_library(recorder_sources_lib OBJECT ${RECORDER_SOURCES})

set(COMMON_SOURCES
  ${RADIO_SOURCES}
  src/agora/stats.cc
  src/common/phy_stats.cc
  src/common/framestats.cc
  src/agora/doencode.cc
  src/common/utils.cc
  src/common/config.cc
  src/common/comms-lib.cc
  src/common/comms-lib-avx.cc
  src/common/signal_handler.cc
  src/common/modulation.cc
  src/common/modulation_srslte.cc
  src/common/net.cc
  src/common/crc.cc
  src/common/memory_manage.cc
  src/common/scrambler.cc
  src/mac/mac_scheduler.cc
  src/common/ipc/udp_comm.cc
  src/common/ipc/network_utils.cc
  src/common/loggers/csv_logger.cc
  src/common/loggers/mat_logger.cc
  src/encoder/cyclic_shift.cc
  src/encoder/encoder.cc
  src/encoder/iobuffer.cc
  src/data_generator/data_generator.cc)
add_library(common_sources_lib OBJECT ${COMMON_SOURCES})

set(SHARED_TXRX_SOURCES
  src/agora/txrx/packet_txrx.cc
  src/agora/txrx/workers/txrx_worker.cc)
add_library(shared_txrx_sources_lib OBJECT ${SHARED_TXRX_SOURCES})

# if(${USE_DPDK})
if(RADIO_TYPE STREQUAL DPDK)
  add_definitions(-DUSE_DPDK)
  set(AGORA_SOURCES
    src/common/dpdk_transport.cc
    src/agora/txrx/packet_txrx_dpdk.cc
    src/agora/txrx/workers/txrx_worker_dpdk.cc)
endif()

set(AGORA_SOURCES ${AGORA_SOURCES}
  ${RADIO_SOURCES_AGORA}
  src/agora/agora.cc
  src/agora/agora_buffer.cc
  src/agora/agora_worker.cc
  src/agora/dofft.cc
  src/agora/doifft.cc
  src/agora/dobroadcast.cc
  src/agora/dobeamweights.cc
  src/agora/dodemul.cc
  src/agora/doprecode.cc
  ${DECODER_SOURCES_AGORA}
  src/mac/mac_thread_basestation.cc
  src/agora/txrx/packet_txrx_sim.cc
  src/agora/txrx/workers/txrx_worker_sim.cc)

add_library(agora_sources_lib OBJECT ${AGORA_SOURCES})

set(CLIENT_SOURCES
  ${RADIO_SOURCES_CLIENT}
  src/client/doifft_client.cc
  src/client/dodecode_client.cc
  src/client/ue_worker.cc
  src/client/phy-ue.cc
  src/client/txrx/packet_txrx_client_sim.cc
  src/client/txrx/workers/txrx_worker_client_sim.cc
  src/mac/mac_thread_client.cc)
add_library(client_sources_lib OBJECT ${CLIENT_SOURCES})

set(COMMON_LIBS -Wl,--start-group ${MKL_LIBS} ${BLAS_LIBRARIES} -Wl,--end-group ${NUMA_LIBRARIES} ${FLEXRAN_LDPC_LIBS} ${HDF5_LIBRARIES} ${DPDK_LIBRARIES} ${ARMADILLO_LIBRARIES} ${SOAPY_LIB}
    ${PYTHON_LIB} ${Boost_LIBRARIES} ${GFLAGS_LIBRARIES} ${UHD_LIBRARIES} ${COMMON_LIBS})
message(VERBOSE "Common libs: ${COMMON_LIBS}")

# TODO: The main agora executable is performance-critical, so we need to
# test if compiling against precompiled objects instead of compiling directly
# against sources reduces performance.
add_executable(agora
  src/agora/main.cc
  $<TARGET_OBJECTS:recorder_sources_lib>
  $<TARGET_OBJECTS:agora_sources_lib>
  $<TARGET_OBJECTS:shared_txrx_sources_lib>
  $<TARGET_OBJECTS:common_sources_lib>)
target_link_libraries(agora ${COMMON_LIBS})

add_executable(data_generator
  ${CMAKE_CURRENT_SOURCE_DIR}/src/data_generator/data_generator_main.cc
  $<TARGET_OBJECTS:common_sources_lib>)
target_link_libraries(data_generator ${COMMON_LIBS})
target_compile_definitions(data_generator PRIVATE GENERATE_DATA)

add_executable(user
  src/client/user-main.cc
  $<TARGET_OBJECTS:recorder_sources_lib>
  $<TARGET_OBJECTS:client_sources_lib>
  $<TARGET_OBJECTS:shared_txrx_sources_lib>
  $<TARGET_OBJECTS:common_sources_lib>)
target_link_libraries(user ${COMMON_LIBS})

add_executable(sender
  simulator/sender_cli.cc
  simulator/sender.cc
  src/common/dpdk_transport.cc
  $<TARGET_OBJECTS:common_sources_lib>)
target_link_libraries(sender ${COMMON_LIBS})

add_executable(sim
  simulator/main.cc
  simulator/simulator.cc
  simulator/sender.cc
  simulator/receiver.cc
  src/common/dpdk_transport.cc
  $<TARGET_OBJECTS:common_sources_lib>)
target_link_libraries(sim ${COMMON_LIBS})

add_executable(chsim
  simulator/chsim_main.cc
  simulator/channel_sim.cc
  simulator/channel.cc
  $<TARGET_OBJECTS:common_sources_lib>)
target_link_libraries(chsim ${COMMON_LIBS})

add_executable(macuser
     ${MAC_CLIENT_SOURCES}
     ${COMMON_SOURCES})
target_link_libraries(macuser ${COMMON_LIBS})

add_executable(macbs
     ${MAC_BS_SOURCES}
     ${COMMON_SOURCES})
target_link_libraries(macbs ${COMMON_LIBS})

if(RADIO_TYPE STREQUAL SOAPY_IRIS)
  add_executable(radio_test
      test/radio_test/main.cc
      $<TARGET_OBJECTS:recorder_sources_lib>
      $<TARGET_OBJECTS:agora_sources_lib>
      $<TARGET_OBJECTS:client_sources_lib>
      $<TARGET_OBJECTS:shared_txrx_sources_lib>
      $<TARGET_OBJECTS:common_sources_lib>)
  target_link_libraries(radio_test ${COMMON_LIBS})

  add_executable(calibrate
    ${CMAKE_CURRENT_SOURCE_DIR}/src/radio/radio_set/calibrate_main.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/radio/radio_set/radio_set_calibrate.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/radio/radio_set/radio_calibrate_digital.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/src/radio/radio_set/radio_calibrate_analog.cc
    $<TARGET_OBJECTS:common_sources_lib>)
  target_link_libraries(calibrate ${COMMON_LIBS})
  target_compile_definitions(calibrate PRIVATE CALIBRATE)
endif()

# End-to-end test
add_executable(test_agora
  test/test_agora/main.cc
  $<TARGET_OBJECTS:recorder_sources_lib>
  $<TARGET_OBJECTS:agora_sources_lib>
  $<TARGET_OBJECTS:shared_txrx_sources_lib>
  $<TARGET_OBJECTS:common_sources_lib>)
target_link_libraries(test_agora ${COMMON_LIBS})

set(LDPC_TESTS test_ldpc test_ldpc_mod test_ldpc_baseband test_pktmbuf_pool_create)
foreach(test_name IN LISTS LDPC_TESTS)
  add_executable(${test_name}
    test/compute_kernels/ldpc/${test_name}.cc
    $<TARGET_OBJECTS:common_sources_lib>)
  # Unit tests should pass without memory errors
  set_target_properties(${test_name} PROPERTIES CMAKE_CXX_FLAGS "-fsanitize=address")
  target_link_libraries(${test_name} ${COMMON_LIBS} ${GTEST_LIBRARIES})
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

# Unit tests
set(UNIT_TESTS test_armadillo test_datatype_conversion test_udp_client_server
  test_concurrent_queue test_zf test_zf_threaded test_demul_threaded 
  test_ptr_grid test_avx512_complex_mul test_scrambler
  test_256qam_demod test_ctrl_channel test_equal test_equal_time test_batch_mm
  test_batch_mat_inv)

foreach(test_name IN LISTS UNIT_TESTS)
  add_executable(${test_name}
    test/unit_tests/${test_name}.cc
    $<TARGET_OBJECTS:recorder_sources_lib>
    $<TARGET_OBJECTS:agora_sources_lib>
    $<TARGET_OBJECTS:shared_txrx_sources_lib>
    $<TARGET_OBJECTS:common_sources_lib>)

  # Unit tests should pass without memory errors
  set_target_properties(${test_name} PROPERTIES CMAKE_CXX_FLAGS "-fsanitize=address")
  target_link_libraries(${test_name} ${COMMON_LIBS} ${GTEST_LIBRARIES})
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()
