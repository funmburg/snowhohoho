cmake_minimum_required(VERSION 3.20)
project(Snegopad LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Статическая сборка: готовый .exe не требует установки Visual C++ Redistributable
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_executable(Snegopad WIN32 src/main.cpp res/app.rc)
target_compile_definitions(Snegopad PRIVATE UNICODE _UNICODE)
if(MSVC)
  target_compile_options(Snegopad PRIVATE /utf-8 /W3 /permissive-)
endif()
target_link_libraries(Snegopad PRIVATE
  d3d11 dxgi d2d1 dcomp dwmapi wtsapi32 shell32 user32 gdi32 advapi32)
