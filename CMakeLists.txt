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

set(VITA_APP_NAME  "ComboPeel")
set(VITA_TITLEID   "CPEL00001")
set(VITA_VERSION   "01.00")

# ---------------- Plugin de kernel ----------------
add_executable(combo_peel src/main.c)
target_compile_options(combo_peel PRIVATE -Wall -O2 -std=gnu99)
set_target_properties(combo_peel PROPERTIES LINK_FLAGS "-Wl,-q -nostdlib")
target_link_libraries(combo_peel
  taihenForKernel_stub
  SceThreadmgrForDriver_stub
  SceCtrlForDriver_stub
  ScePowerForDriver_stub
  gcc
)
vita_create_self(combo_peel.skprx combo_peel CONFIG exports.yml UNSAFE)

# ---------------- Instalador (VPK) ----------------
add_executable(installer installer/main.c)
target_compile_options(installer PRIVATE -Wall -O2 -std=gnu99)
set_target_properties(installer PROPERTIES LINK_FLAGS "-Wl,-q")
target_link_libraries(installer
  vita2d
  SceDisplay_stub
  SceGxm_stub
  SceSysmodule_stub
  SceCtrl_stub
  ScePgf_stub
  ScePvf_stub
  SceCommonDialog_stub
  SceAppMgr_stub
  SceAppUtil_stub
  ScePower_stub
  SceLibKernel_stub
  SceIofilemgr_stub
  m
)
vita_create_self(eboot.bin installer UNSAFE)

vita_create_vpk(ComboPeel.vpk ${VITA_TITLEID} eboot.bin
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${CMAKE_SOURCE_DIR}/installer/sce_sys/icon0.png sce_sys/icon0.png
  FILE ${CMAKE_SOURCE_DIR}/installer/sce_sys/livearea/contents/bg.png sce_sys/livearea/contents/bg.png
  FILE ${CMAKE_SOURCE_DIR}/installer/sce_sys/livearea/contents/startup.png sce_sys/livearea/contents/startup.png
  FILE ${CMAKE_SOURCE_DIR}/installer/sce_sys/livearea/contents/template.xml sce_sys/livearea/contents/template.xml
  FILE ${CMAKE_BINARY_DIR}/combo_peel.skprx combo_peel.skprx
)
add_dependencies(ComboPeel.vpk-vpk combo_peel.skprx-self)
