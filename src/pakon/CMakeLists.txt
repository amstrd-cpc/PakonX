# Core library: pakon_core
#
# Layering (top depends on bottom):
#   scanner → ppb → usb
#   scan → stream, image, protocol, ppb, eeprom  (the OEM scan state
#   machine over ICommandChannel + IImageStream; no direct USB I/O)
#   stream → usb (IBulkInPipe only)
#   bootstrap → usb   (cold-device read-only probe; no PPB involvement)
#   all layers use errors/ and logging/
# protocol/ is header-only constants, documented against pakon-reference.

set(PAKON_CORE_SOURCES
    errors/error.cpp
    logging/logger.cpp
    usb/transport.cpp
    bootstrap/probe.cpp
    bootstrap/fx2_loader.cpp
    ppb/packet.cpp
    ppb/client.cpp
    scanner/scanner.cpp
    scan/device.cpp
    scan/lines.cpp
    scan/corrections.cpp
    scan/runner.cpp
    scan/f235_plan.cpp
    scan/sidecar.cpp
    image/raw_writer.cpp
    image/tiff.cpp
    image/bmp.cpp
    image/jpeg.cpp
    stream/image_stream.cpp
    eeprom/eeprom.cpp
)

if(WIN32)
    list(APPEND PAKON_CORE_SOURCES
        usb/enumerate_win.cpp
        usb/win_usb_transport.cpp
        usb/driver_attrib_win.cpp
        usb/descriptors_win.cpp
    )
else()
    # Linux / macOS: the libusb-1.0 backend when it is installed
    # (usb/libusb_backend.cpp), else the stub (enumeration empty,
    # open_first usb_not_supported) so the platform-independent layers
    # still build and test. Driver attribution and descriptor discovery
    # are Windows diagnostics: stubs everywhere else.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBUSB IMPORTED_TARGET libusb-1.0)
    endif()
    if(LIBUSB_FOUND)
        list(APPEND PAKON_CORE_SOURCES usb/libusb_backend.cpp)
    else()
        message(STATUS "libusb-1.0 not found: pakon-cli builds without a USB backend")
        list(APPEND PAKON_CORE_SOURCES usb/transport_stub.cpp)
    endif()
    list(APPEND PAKON_CORE_SOURCES
        usb/driver_attrib_stub.cpp
        usb/descriptors_stub.cpp
    )
endif()

add_library(pakon_core STATIC ${PAKON_CORE_SOURCES})

if(NOT WIN32 AND LIBUSB_FOUND)
    target_link_libraries(pakon_core PUBLIC PkgConfig::LIBUSB)
    if(APPLE)
        # A static libusb (the macOS release build) needs these from the system.
        target_link_libraries(pakon_core PUBLIC "-framework IOKit" "-framework CoreFoundation" "-framework Security")
    endif()
endif()

target_include_directories(pakon_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

if(WIN32)
    # Windows system import libraries used by usb/enumerate_win.cpp
    # (SetupAPI enumeration) and usb/win_usb_transport.cpp (WinUSB pipe
    # I/O). PRIVATE keeps them off the compile interface; for a static
    # library CMake still propagates them to consumers as
    # $<LINK_ONLY:...>, so executables linking pakon_core resolve them
    # at link time. No SDK paths are hardcoded - the MSVC toolchain
    # resolves these by name.
    target_link_libraries(pakon_core PRIVATE
        setupapi
        winusb
        ole32
    )

    # GUID_DEVINTERFACE_USB_DEVICE is declared by usbiodef.h but defined
    # by no SDK library (verified by scanning all um/x64 import libs);
    # the header documentation requires "include initguid.h first, then
    # include usbiodef.h" so this translation unit emits the definition.
    # The forced include must precede everything else because guiddef.h
    # latches the INITGUID-dependent DEFINE_GUID form at first inclusion
    # (its defining form is DECLSPEC_SELECTANY, so no duplicate-symbol
    # risk). Source files left unmodified; GNU toolchains resolve the
    # GUID from their import libraries and need no forced include.
    set_source_files_properties(usb/enumerate_win.cpp PROPERTIES
        COMPILE_OPTIONS "$<$<CXX_COMPILER_ID:MSVC,Clang>:/FIinitguid.h>"
    )
endif()

# Processing library: pakon_process — offline work on scan outputs
# (.pakraw + .scan.json): frame separation now, the image pipeline
# later. Depends on errors/ and image/ only, never on scan/ or USB, so
# processing stays out of the scan runner.
add_library(pakon_process STATIC
    process/json.cpp
    process/pakraw.cpp
    process/frames.cpp
    process/positive.cpp
    process/ice.cpp
    process/filmid.cpp
    process/look.cpp
    process/dx.cpp
)
find_package(Threads REQUIRED) # develop_frame works in bands of lines
target_link_libraries(pakon_process PUBLIC pakon_core Threads::Threads)
