cmake_minimum_required(VERSION 3.10)
project(connect-to-calibre)

# Установка стандарта C++
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Путь к PocketBook SDK
set(TOOLCHAIN_PATH "${CMAKE_SOURCE_DIR}/SDK/SDK_6.3.0/SDK-B288" CACHE PATH "Path to PocketBook SDK")

# Установка файла toolchain для кросс-компиляции
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_SOURCE_DIR}/toolchain.cmake")
endif()

# Директории заголовочных файлов
include_directories(
    ${TOOLCHAIN_PATH}/usr/arm-obreey-linux-gnueabi/sysroot/usr/include
    ${TOOLCHAIN_PATH}/usr/arm-obreey-linux-gnueabi/sysroot/usr/include/freetype2
    ${CMAKE_SOURCE_DIR}/src
)

# Директории библиотек
link_directories(
    ${TOOLCHAIN_PATH}/usr/arm-obreey-linux-gnueabi/sysroot/usr/lib
)

# Исходные файлы
set(SOURCES
    src/main.cpp
)

# Создание исполняемого файла
add_executable(${PROJECT_NAME} ${SOURCES})

# Линковка библиотек
target_link_libraries(${PROJECT_NAME}
    inkview
    freetype
)

# Установка имени выходного файла
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "connect-to-calibre.app")

# Установка
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION .
)
