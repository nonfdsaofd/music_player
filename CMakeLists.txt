cmake_minimum_required(VERSION 3.0.0)
project(main VERSION 0.1.0)

include(CTest)
enable_testing()

add_executable(main main.cpp)
target_link_options(main PRIVATE -mwindows)
target_link_libraries(main PRIVATE comctl32)
target_link_libraries(main PRIVATE winmm)
target_link_libraries(main PRIVATE imm32)
target_link_options(main PRIVATE
    -static-libgcc        # 静态链接GCC运行时库
    -static-libstdc++     # 静态链接C++标准库
    -mthreads             # 静态链接winpthread（必加！）
)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -finput-charset=UTF-8 -fexec-charset=GBK")
