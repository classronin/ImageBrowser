cmake_minimum_required(VERSION 3.20)
project(ImageBrowser CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")

set(JPEGVIEW_DIR ${CMAKE_SOURCE_DIR}/src/JPEGView)

# JPEGView 内核里保留的部分：解码、处理、设置、异步加载。
set(JPEGVIEW_SOURCES
  ${JPEGVIEW_DIR}/stdafx.cpp
  ${JPEGVIEW_DIR}/ApplyFilterAVX.cpp
  ${JPEGVIEW_DIR}/BasicProcessing.cpp
  ${JPEGVIEW_DIR}/DirectoryWatcher.cpp
  ${JPEGVIEW_DIR}/EXIFHelpers.cpp
  ${JPEGVIEW_DIR}/EXIFReader.cpp
  ${JPEGVIEW_DIR}/FileList.cpp
  ${JPEGVIEW_DIR}/HashCompareLPCTSTR.cpp
  ${JPEGVIEW_DIR}/Helpers.cpp
  ${JPEGVIEW_DIR}/HistogramCorr.cpp
  ${JPEGVIEW_DIR}/ICCProfileTransform.cpp
  ${JPEGVIEW_DIR}/ImageLoadThread.cpp
  ${JPEGVIEW_DIR}/JPEGImage.cpp
  ${JPEGVIEW_DIR}/JPEGProvider.cpp
  ${JPEGVIEW_DIR}/KeyMap.cpp
  ${JPEGVIEW_DIR}/LocalDensityCorr.cpp
  ${JPEGVIEW_DIR}/MultiMonitorSupport.cpp
  ${JPEGVIEW_DIR}/NLS.cpp
  ${JPEGVIEW_DIR}/ParameterDB.cpp
  ${JPEGVIEW_DIR}/ProcessingThreadPool.cpp
  ${JPEGVIEW_DIR}/UserCommand.cpp
  ${JPEGVIEW_DIR}/PNGWrapper.cpp
  ${JPEGVIEW_DIR}/PSDWrapper.cpp
  ${JPEGVIEW_DIR}/QOIWrapper.cpp
  ${JPEGVIEW_DIR}/ReaderBMP.cpp
  ${JPEGVIEW_DIR}/ReaderTGA.cpp
  ${JPEGVIEW_DIR}/ReaderORA.cpp
  ${JPEGVIEW_DIR}/ResizeFilter.cpp
  ${JPEGVIEW_DIR}/SettingsProvider.cpp
  ${JPEGVIEW_DIR}/TJPEGWrapper.cpp
  ${JPEGVIEW_DIR}/WEBPWrapper.cpp
  ${JPEGVIEW_DIR}/WorkThread.cpp
  ${JPEGVIEW_DIR}/XMMImage.cpp
)

set(APP_SOURCES
  src/ImageBrowser.cpp
  src/FormatStubs.cpp
  src/ColorPicker.cpp
  ImageBrowser.rc
)

set(ATL_INCLUDE
  "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207/atlmfc/include")

add_executable(ImageBrowser WIN32 ${APP_SOURCES} ${JPEGVIEW_SOURCES})

target_include_directories(ImageBrowser PRIVATE
  src
  ${JPEGVIEW_DIR}
  ${CMAKE_SOURCE_DIR}/deps/WTL-sf/Include
  ${JPEGVIEW_DIR}/libjpeg-turbo/include
  ${JPEGVIEW_DIR}/libpng-apng/include
  ${JPEGVIEW_DIR}/libwebp/include
  ${JPEGVIEW_DIR}/qoi
  E:/Programs/vcpkg/installed/x64-windows-static/include
  ${ATL_INCLUDE}
)

target_compile_definitions(ImageBrowser PRIVATE
  UNICODE _UNICODE
  _CRT_SECURE_NO_WARNINGS
  WIN32_LEAN_AND_MEAN
)

target_compile_options(ImageBrowser PRIVATE /utf-8 /W3 /EHsc)
# 与 JPEGView 原工程一致：AVX 重采样实现单独开 /arch:AVX（原 vcxproj 里 ApplyFilterAVX.cpp 就是 AdvancedVectorExtensions）。
set_source_files_properties(${JPEGVIEW_DIR}/ApplyFilterAVX.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX")

target_link_directories(ImageBrowser PRIVATE
  ${JPEGVIEW_DIR}/libjpeg-turbo/lib64
  ${JPEGVIEW_DIR}/libpng-apng/lib64
  ${JPEGVIEW_DIR}/libwebp/lib64
  E:/Programs/vcpkg/installed/x64-windows-static/lib
)

target_link_libraries(ImageBrowser PRIVATE
  turbojpeg-static
  libpng16
  zlib
  libwebp
  libwebpdemux
  lcms2
  gdiplus
  shlwapi
  ole32
  oleaut32
  uuid
  user32
  gdi32
  shell32
  advapi32
    winmm
)




