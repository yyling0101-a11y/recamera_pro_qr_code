cmake_minimum_required(VERSION 3.16)
project(qr_pro LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0)
add_executable(qr_pro
    main.cpp
    app_state.cpp
    barcode.cpp
    camera.cpp
    services.cpp
)
target_include_directories(qr_pro PRIVATE ${RECAMERA_SYSROOT}/usr/include/opencv4)
target_link_libraries(qr_pro PRIVATE PkgConfig::GST opencv_core opencv_imgproc opencv_imgcodecs zbar mosquitto pthread)
target_link_directories(qr_pro PRIVATE ${RECAMERA_SYSROOT}/usr/lib64)
set_target_properties(qr_pro PROPERTIES BUILD_RPATH "/oem/usr/lib" INSTALL_RPATH "/oem/usr/lib")
