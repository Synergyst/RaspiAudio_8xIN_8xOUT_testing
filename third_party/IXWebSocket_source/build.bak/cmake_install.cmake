# Install script for directory: /tmp/IXWebSocket

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "/usr/local")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Install shared libraries without execute permission?
if(NOT DEFINED CMAKE_INSTALL_SO_NO_EXE)
  set(CMAKE_INSTALL_SO_NO_EXE "1")
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

# Set path to fallback-tool for dependency-resolution.
if(NOT DEFINED CMAKE_OBJDUMP)
  set(CMAKE_OBJDUMP "/usr/bin/objdump")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY FILES "/tmp/IXWebSocket/build/libixwebsocket.a")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include/ixwebsocket" TYPE FILE FILES
    "/tmp/IXWebSocket/ixwebsocket/IXBase64.h"
    "/tmp/IXWebSocket/ixwebsocket/IXBench.h"
    "/tmp/IXWebSocket/ixwebsocket/IXCancellationRequest.h"
    "/tmp/IXWebSocket/ixwebsocket/IXConnectionState.h"
    "/tmp/IXWebSocket/ixwebsocket/IXDNSLookup.h"
    "/tmp/IXWebSocket/ixwebsocket/IXExponentialBackoff.h"
    "/tmp/IXWebSocket/ixwebsocket/IXGetFreePort.h"
    "/tmp/IXWebSocket/ixwebsocket/IXGzipCodec.h"
    "/tmp/IXWebSocket/ixwebsocket/IXHttp.h"
    "/tmp/IXWebSocket/ixwebsocket/IXHttpClient.h"
    "/tmp/IXWebSocket/ixwebsocket/IXHttpServer.h"
    "/tmp/IXWebSocket/ixwebsocket/IXNetSystem.h"
    "/tmp/IXWebSocket/ixwebsocket/IXProgressCallback.h"
    "/tmp/IXWebSocket/ixwebsocket/IXSelectInterrupt.h"
    "/tmp/IXWebSocket/ixwebsocket/IXSelectInterruptFactory.h"
    "/tmp/IXWebSocket/ixwebsocket/IXSelectInterruptPipe.h"
    "/tmp/IXWebSocket/ixwebsocket/IXSelectInterruptEvent.h"
    "/tmp/IXWebSocket/ixwebsocket/IXSetThreadName.h"
    "/tmp/IXWebSocket/ixwebsocket/IXSocket.h"
    "/tmp/IXWebSocket/ixwebsocket/IXSocketConnect.h"
    "/tmp/IXWebSocket/ixwebsocket/IXSocketFactory.h"
    "/tmp/IXWebSocket/ixwebsocket/IXSocketServer.h"
    "/tmp/IXWebSocket/ixwebsocket/IXSocketTLSOptions.h"
    "/tmp/IXWebSocket/ixwebsocket/IXStrCaseCompare.h"
    "/tmp/IXWebSocket/ixwebsocket/IXUdpSocket.h"
    "/tmp/IXWebSocket/ixwebsocket/IXUniquePtr.h"
    "/tmp/IXWebSocket/ixwebsocket/IXUrlParser.h"
    "/tmp/IXWebSocket/ixwebsocket/IXUuid.h"
    "/tmp/IXWebSocket/ixwebsocket/IXUtf8Validator.h"
    "/tmp/IXWebSocket/ixwebsocket/IXUserAgent.h"
    "/tmp/IXWebSocket/ixwebsocket/IXWebSocket.h"
    "/tmp/IXWebSocket/ixwebsocket/IXWebSocketCloseConstants.h"
    "/tmp/IXWebSocket/ixwebsocket/IXWebSocketCloseInfo.h"
    "/tmp/IXWebSocket/ixwebsocket/IXWebSocketErrorInfo.h"
    "/tmp/IXWebSocket/ixwebsocket/IXWebSocketHandshake.h"
    "/tmp/IXWebSocket/ixwebsocket/IXWebSocketHandshakeKeyGen.h"
    "/tmp/IXWebSocket/ixwebsocket/IXWebSocketHttpHeaders.h"
    "/tmp/IXWebSocket/ixwebsocket/IXWebSocketInitResult.h"
    "/tmp/IXWebSocket/ixwebsocket/IXWebSocketMessage.h"
    "/tmp/IXWebSocket/ixwebsocket/IXWebSocketMessageType.h"
    "/tmp/IXWebSocket/ixwebsocket/IXWebSocketOpenInfo.h"
    "/tmp/IXWebSocket/ixwebsocket/IXWebSocketPerMessageDeflate.h"
    "/tmp/IXWebSocket/ixwebsocket/IXWebSocketPerMessageDeflateCodec.h"
    "/tmp/IXWebSocket/ixwebsocket/IXWebSocketPerMessageDeflateOptions.h"
    "/tmp/IXWebSocket/ixwebsocket/IXWebSocketProxyServer.h"
    "/tmp/IXWebSocket/ixwebsocket/IXWebSocketSendData.h"
    "/tmp/IXWebSocket/ixwebsocket/IXWebSocketSendInfo.h"
    "/tmp/IXWebSocket/ixwebsocket/IXWebSocketServer.h"
    "/tmp/IXWebSocket/ixwebsocket/IXWebSocketTransport.h"
    "/tmp/IXWebSocket/ixwebsocket/IXWebSocketVersion.h"
    "/tmp/IXWebSocket/ixwebsocket/IXSocketOpenSSL.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/ixwebsocket" TYPE FILE FILES "/tmp/IXWebSocket/build/ixwebsocket-config.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/pkgconfig" TYPE FILE FILES "/tmp/IXWebSocket/build/ixwebsocket.pc")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  if(EXISTS "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/ixwebsocket/ixwebsocket-targets.cmake")
    file(DIFFERENT _cmake_export_file_changed FILES
         "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/ixwebsocket/ixwebsocket-targets.cmake"
         "/tmp/IXWebSocket/build/CMakeFiles/Export/dbc99e06a99e696141dafd40631f8060/ixwebsocket-targets.cmake")
    if(_cmake_export_file_changed)
      file(GLOB _cmake_old_config_files "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/ixwebsocket/ixwebsocket-targets-*.cmake")
      if(_cmake_old_config_files)
        string(REPLACE ";" ", " _cmake_old_config_files_text "${_cmake_old_config_files}")
        message(STATUS "Old export file \"$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/lib/cmake/ixwebsocket/ixwebsocket-targets.cmake\" will be replaced.  Removing files [${_cmake_old_config_files_text}].")
        unset(_cmake_old_config_files_text)
        file(REMOVE ${_cmake_old_config_files})
      endif()
      unset(_cmake_old_config_files)
    endif()
    unset(_cmake_export_file_changed)
  endif()
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/ixwebsocket" TYPE FILE FILES "/tmp/IXWebSocket/build/CMakeFiles/Export/dbc99e06a99e696141dafd40631f8060/ixwebsocket-targets.cmake")
  if(CMAKE_INSTALL_CONFIG_NAME MATCHES "^()$")
    file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/ixwebsocket" TYPE FILE FILES "/tmp/IXWebSocket/build/CMakeFiles/Export/dbc99e06a99e696141dafd40631f8060/ixwebsocket-targets-noconfig.cmake")
  endif()
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/tmp/IXWebSocket/build/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
if(CMAKE_INSTALL_COMPONENT)
  if(CMAKE_INSTALL_COMPONENT MATCHES "^[a-zA-Z0-9_.+-]+$")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INSTALL_COMPONENT}.txt")
  else()
    string(MD5 CMAKE_INST_COMP_HASH "${CMAKE_INSTALL_COMPONENT}")
    set(CMAKE_INSTALL_MANIFEST "install_manifest_${CMAKE_INST_COMP_HASH}.txt")
    unset(CMAKE_INST_COMP_HASH)
  endif()
else()
  set(CMAKE_INSTALL_MANIFEST "install_manifest.txt")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "/tmp/IXWebSocket/build/${CMAKE_INSTALL_MANIFEST}"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
