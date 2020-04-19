# For more information about using CMake with Android Studio, read the
# documentation: https://d.android.com/studio/projects/add-native-code.html

# Sets the minimum version of CMake required to build the native library.

cmake_minimum_required(VERSION 3.4.1)

# Creates and names a library, sets it as either STATIC
# or SHARED, and provides the relative paths to its source code.
# You can define multiple libraries, and CMake builds them for you.
# Gradle automatically packages shared libraries with your APK.

include_directories(${CMAKE_SOURCE_DIR}/ffmpeg/include)
include_directories(${CMAKE_SOURCE_DIR}/fdkaac/include)
include_directories(${CMAKE_SOURCE_DIR}/rtmp/include)
include_directories(${CMAKE_SOURCE_DIR}/x264/include)


LINK_DIRECTORIES("${CMAKE_SOURCE_DIR}/libs/${CMAKE_ANDROID_ARCH_ABI}/")

file(GLOB allCpp ${CMAKE_SOURCE_DIR}/*.cpp)

add_library( # Sets the name of the library.
        native-lib

        # Sets the library as a shared library.
        SHARED

        # Provides a relative path to your source file(s).
        ${allCpp})

# Searches for a specified prebuilt library and stores the path as a
# variable. Because CMake includes system libraries in the search path by
# default, you only need to specify the name of the public NDK library
# you want to add. CMake verifies that the library exists before
# completing its build.

find_library( # Sets the name of the path variable.
        log-lib

        # Specifies the name of the NDK library that
        # you want CMake to locate.
        log)

# Specifies libraries CMake should link to your target library. You
# can link multiple libraries, such as libraries you define in this
# build script, prebuilt third-party libraries, or system libraries.

target_link_libraries( # Specifies the target library.
        native-lib

        # Links the target library to the log library
        # included in the NDK.
        -Wl,--start-group
        avcodec avfilter avformat avutil swresample swscale postproc avdevice
        -Wl,--end-group
        z
        rtmp
        x2641
        fdk-aac
        android #渲染屏幕
        OpenSLES #驱动声卡
        ${log-lib})