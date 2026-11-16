cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Defina a variavel VITASDK apontando para o VitaSDK.")
  endif()
endif()

project(combo_peel C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -std=gnu99")
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -nostdlib")

add_executable(combo_peel src/main.c)

target_link_libraries(combo_peel
  taihenForKernel_stub
  SceThreadmgrForDriver_stub
  SceCtrlForDriver_stub
  ScePowerForDriver_stub
  gcc
)

vita_create_self(combo_peel.skprx combo_peel CONFIG exports.yml UNSAFE)
