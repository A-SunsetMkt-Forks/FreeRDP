# FreeRDP: A Remote Desktop Protocol Implementation
# FreeRDP Windows Server cmake build script
#
# Copyright 2012 Marc-Andre Moreau <marcandre.moreau@gmail.com>
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set(MODULE_NAME "wfreerdp-server")
set(MODULE_PREFIX "FREERDP_SERVER_WINDOWS")

include(WarnUnmaintained)
warn_unmaintained(${MODULE_NAME} "-DWITH_PLATFORM_SERVER=OFF")

include_directories(.)

set(${MODULE_PREFIX}_SRCS
    wf_update.c
    wf_update.h
    wf_dxgi.c
    wf_dxgi.h
    wf_input.c
    wf_input.h
    wf_interface.c
    wf_interface.h
    wf_mirage.c
    wf_mirage.h
    wf_peer.c
    wf_peer.h
    wf_settings.c
    wf_settings.h
    wf_info.c
    wf_info.h
)

if(CHANNEL_RDPSND AND NOT WITH_RDPSND_DSOUND)
  set(${MODULE_PREFIX}_SRCS ${${MODULE_PREFIX}_SRCS} wf_rdpsnd.c wf_rdpsnd.h wf_wasapi.c wf_wasapi.h)
endif()

if(CHANNEL_RDPSND AND WITH_RDPSND_DSOUND)
  set(${MODULE_PREFIX}_SRCS ${${MODULE_PREFIX}_SRCS} wf_rdpsnd.c wf_rdpsnd.h wf_directsound.c wf_directsound.h)
endif()

if(WITH_SERVER_INTERFACE)
  addtargetwithresourcefile(${MODULE_NAME} FALSE "${FREERDP_VERSION}" ${MODULE_PREFIX}_SRCS)
  target_include_directories(${MODULE_NAME} INTERFACE $<INSTALL_INTERFACE:include>)
else()
  list(APPEND ${MODULE_PREFIX}_SRCS cli/wfreerdp.c cli/wfreerdp.h)
  addtargetwithresourcefile(${MODULE_NAME} TRUE "${FREERDP_VERSION}" ${MODULE_PREFIX}_SRCS)
endif()

if(NOT CMAKE_WINDOWS_VERSION STREQUAL "WINXP")
  set(${MODULE_PREFIX}_LIBS ${${MODULE_PREFIX}_LIBS} d3d11 dxgi)
endif()

set(${MODULE_PREFIX}_LIBS ${${MODULE_PREFIX}_LIBS} dsound)
set(${MODULE_PREFIX}_LIBS ${${MODULE_PREFIX}_LIBS} freerdp-server freerdp)

target_link_libraries(${MODULE_NAME} ${${MODULE_PREFIX}_LIBS})

if(WITH_SERVER_INTERFACE)
  install(TARGETS ${MODULE_NAME} COMPONENT libraries ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
          LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  )
else()
  install(TARGETS ${MODULE_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT server)
endif()

if(WITH_SERVER_INTERFACE)
  add_subdirectory(cli)
endif()

set_property(TARGET ${MODULE_NAME} PROPERTY FOLDER "Server/Windows")
