cmake_minimum_required(VERSION 3.19)
project(carfish C CXX) # C needed so CMAKE_C_COMPILER drives openh264's NEON asm

set(CMAKE_CXX_STANDARD 17) # protobuf >= 22 / abseil
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# DESKTOP_BUILD: build a headless binary on a regular Linux PC (no Silica,
# no sailfishapp) to test the protocol and media pipeline against a fake HU.
option(DESKTOP_BUILD "Headless host build for testing" OFF)

find_package(Qt5 COMPONENTS Core Network DBus Qml Gui Quick Sql REQUIRED)

include(FindPkgConfig)
set(DEP_MODULES
    openssl
    protobuf-lite
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-video-1.0
)
if(DESKTOP_BUILD)
    add_compile_definitions(CARFISH_DESKTOP)
else()
    pkg_search_module(SAILFISH sailfishapp REQUIRED)
    # Qt on Sailfish is built against GLES2 and calls it directly.
    list(APPEND DEP_MODULES glesv2)
endif()
pkg_check_modules(DEPS REQUIRED IMPORTED_TARGET ${DEP_MODULES})

# Device-only telephony/contacts backends for the phone (dialer) pane:
#   Qt5Contacts    -> the address book (qtcontacts-sqlite)
#   commhistory-qt5 -> the recent-call log
# On the host build these are #ifdef'd out and replaced by a small mock.
if(NOT DESKTOP_BUILD)
    pkg_check_modules(PHONE REQUIRED IMPORTED_TARGET Qt5Contacts commhistory-qt5)
    set(PHONE_LINK PkgConfig::PHONE)
endif()

find_program(PROTOC protoc REQUIRED)

# Bundled openh264 (Constrained Baseline H.264 encoder). We ship it inside the
# package so the app needs no system H.264 encoder on the device (the MediaTek
# Codec2 HW encoder can't be driven through gst-droid's legacy metadata path).
#   - Host (DESKTOP_BUILD): link the system openh264 shared library.
#   - Device target: cross/native-build the vendored source into a static lib
#     during %build and link it statically, so nothing extra ships or is Require'd.
# Either way the API headers come from the vendored tree for a single source.
set(OPENH264_SRC ${CMAKE_SOURCE_DIR}/thirdparty/openh264)
set(OPENH264_INCLUDE ${OPENH264_SRC}/codec/api)
if(DESKTOP_BUILD)
    pkg_check_modules(OPENH264 REQUIRED IMPORTED_TARGET openh264)
    set(OPENH264_LINK PkgConfig::OPENH264)
else()
    # Build out of tree (objects under the CMake build dir) so the source tree
    # and the Source0 tarball stay clean. openh264's Makefile honours SRC_PATH
    # + vpath and creates its own object subdirs under the working directory.
    set(OPENH264_OBJDIR ${CMAKE_BINARY_DIR}/openh264-obj)
    set(OPENH264_LIB ${OPENH264_OBJDIR}/libopenh264.a)
    file(MAKE_DIRECTORY ${OPENH264_OBJDIR})
    add_custom_command(
        OUTPUT ${OPENH264_LIB}
        COMMAND ${CMAKE_COMMAND} -E env make -f ${OPENH264_SRC}/Makefile
                SRC_PATH=${OPENH264_SRC}/ libopenh264.a
                CC=${CMAKE_C_COMPILER} CXX=${CMAKE_CXX_COMPILER}
                CCAS=${CMAKE_C_COMPILER} AR=${CMAKE_AR}
                BUILDTYPE=Release V=No
        WORKING_DIRECTORY ${OPENH264_OBJDIR}
        COMMENT "Building bundled openh264 static library"
        VERBATIM)
    add_custom_target(openh264_build DEPENDS ${OPENH264_LIB})
    set(OPENH264_LINK ${OPENH264_LIB})
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# Generate protobuf-lite sources
set(PROTO_FILES
    ${CMAKE_SOURCE_DIR}/proto/aap.proto
    ${CMAKE_SOURCE_DIR}/proto/wireless.proto
)
set(PROTO_GEN_DIR ${CMAKE_BINARY_DIR}/proto-gen)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})
set(PROTO_SRCS)
foreach(proto ${PROTO_FILES})
    get_filename_component(name ${proto} NAME_WE)
    list(APPEND PROTO_SRCS ${PROTO_GEN_DIR}/${name}.pb.cc)
    add_custom_command(
        OUTPUT ${PROTO_GEN_DIR}/${name}.pb.cc ${PROTO_GEN_DIR}/${name}.pb.h
        COMMAND ${PROTOC} --cpp_out=${PROTO_GEN_DIR} -I ${CMAKE_SOURCE_DIR}/proto ${proto}
        DEPENDS ${proto}
        COMMENT "protoc ${name}.proto")
endforeach()

add_executable(carfish
    src/main.cpp
    src/controller.cpp
    src/apps/appregistry.cpp
    src/apps/mpriscontroller.cpp
    src/apps/phonemanager.cpp
    src/aap/framer.cpp
    src/aap/cryptor.cpp
    src/aap/session.cpp
    src/transport/transport.h
    src/transport/tcptransport.cpp
    src/transport/usbffstransport.cpp
    src/transport/usbhelperclient.cpp
    src/wireless/bluezprofile.cpp
    src/wireless/wifijoiner.cpp
    src/wireless/wirelesssetup.cpp
    src/media/videosource.cpp
    src/media/audiosource.cpp
    src/media/micreceiver.cpp
    src/media/carscreen.cpp
    src/media/navbridge.cpp
    src/util/credentials.cpp
    ${PROTO_SRCS}
)
target_compile_definitions(carfish PRIVATE
    $<$<OR:$<CONFIG:Debug>,$<CONFIG:RelWithDebInfo>>:QT_QML_DEBUG>
)
target_include_directories(carfish PRIVATE
    ${PROTO_GEN_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${OPENH264_INCLUDE}
    ${SAILFISH_INCLUDE_DIRS}
)
if(NOT DESKTOP_BUILD)
    add_dependencies(carfish openh264_build)
endif()
target_link_libraries(carfish
    Qt5::Quick
    Qt5::Network
    Qt5::DBus
    PkgConfig::DEPS
    ${PHONE_LINK}
    ${OPENH264_LINK}
    ${SAILFISH_LDFLAGS}
)

# Desktop-only: an interactive preview of the projected car UI on a computer.
if(DESKTOP_BUILD)
    add_executable(carfish-preview
        tools/preview/preview.cpp
        src/apps/phonemanager.cpp)
    target_compile_definitions(carfish-preview PRIVATE
        CARMAIN_QML="${CMAKE_SOURCE_DIR}/carqml/CarMain.qml"
        PREVIEW_QML="${CMAKE_SOURCE_DIR}/tools/preview/preview.qml")
    target_include_directories(carfish-preview PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(carfish-preview Qt5::Quick Qt5::Gui Qt5::DBus)
endif()

# Device-only: the privileged system helper. It runs as root (via systemd) and
# drives connman, so the unprivileged app can join the car's WiFi and keep the
# internet on cellular on a stock Sailfish OS.
if(NOT DESKTOP_BUILD)
    add_executable(carfish-helper
        helper/helpermain.cpp
        helper/wifihelper.cpp
        helper/usbgadget.cpp
        helper/contactshelper.cpp)
    target_link_libraries(carfish-helper Qt5::Core Qt5::DBus Qt5::Sql)
    install(TARGETS carfish-helper RUNTIME DESTINATION bin)
endif()

install(TARGETS carfish RUNTIME DESTINATION bin)
install(DIRECTORY qml DESTINATION share/carfish)
install(DIRECTORY carqml DESTINATION share/carfish)
install(DIRECTORY config DESTINATION share/carfish USE_SOURCE_PERMISSIONS)
install(FILES carfish.desktop DESTINATION share/applications)
foreach(size 86x86 108x108 128x128 172x172)
    install(FILES icons/${size}/carfish.png DESTINATION share/icons/hicolor/${size}/apps)
endforeach()

add_custom_target(distfiles
    SOURCES
        carfish.desktop
        rpm/carfish.spec
        ${PROTO_FILES}
)

file(WRITE "${CMAKE_BINARY_DIR}/QtCreatorDeployment.txt"
    "${CMAKE_INSTALL_PREFIX}\n${CMAKE_BINARY_DIR}/carfish:bin\n")
