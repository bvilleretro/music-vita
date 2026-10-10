cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK is not set")
  endif()
endif()

project(MusicVita C)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Music Vita")
set(VITA_TITLEID  "MUSC00001")
set(VITA_VERSION  "01.00")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O3 -std=gnu99")

add_executable(MusicVita src/main.c)

target_link_libraries(MusicVita
  vita2d
  SceLibKernel_stub
  SceDisplay_stub
  SceGxm_stub
  SceSysmodule_stub
  SceCtrl_stub
  SceAudio_stub
  SceAppMgr_stub
  ScePower_stub
  ScePgf_stub
  ScePvf_stub
  SceCommonDialog_stub
  freetype png jpeg bz2 z m c
)

vita_create_self(MusicVita.self MusicVita)

# Art/font files are packed only if they exist
set(VPK_FILES "")
foreach(f IN ITEMS
    sce_sys/icon0.png
    sce_sys/pic0.png
    sce_sys/livearea/contents/bg.png
    sce_sys/livearea/contents/startup.png
    sce_sys/livearea/contents/template.xml
    assets/font.ttf
    assets/logo.png
    assets/splash_bg.png)
  if(EXISTS "${CMAKE_SOURCE_DIR}/${f}")
    list(APPEND VPK_FILES FILE "${CMAKE_SOURCE_DIR}/${f}" "${f}")
  endif()
endforeach()

vita_create_vpk(MusicVita.vpk ${VITA_TITLEID} MusicVita.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  ${VPK_FILES}
)
