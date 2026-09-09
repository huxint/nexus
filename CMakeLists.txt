cmake_minimum_required(VERSION 3.28)
project(nexus CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
file(CREATE_LINK "${CMAKE_BINARY_DIR}/compile_commands.json"
     "${CMAKE_CURRENT_SOURCE_DIR}/compile_commands.json" SYMBOLIC)

find_package(Threads REQUIRED)
include(CTest)

# -DSANITIZER=address|thread, UBSan always layered on top
set(SANITIZER "" CACHE STRING "sanitizer: address or thread")
if(SANITIZER)
    add_compile_options(-fsanitize=${SANITIZER},undefined -fno-omit-frame-pointer -g
                        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${SANITIZER},undefined)
endif()

# Header-only library. 公共头含契约语法: 无 -fcontracts 时可编译但链接期缺
# handle_contract_violation, 故经 INTERFACE 注入该 flag(仅 GCC, Clang 无此选项);
# 契约求值语义不下发, 由下游自选
add_library(nexus INTERFACE)
add_library(huxint::nexus ALIAS nexus)
target_include_directories(nexus INTERFACE include)
target_compile_features(nexus INTERFACE cxx_std_26)
target_link_libraries(nexus INTERFACE Threads::Threads)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(nexus INTERFACE -fcontracts)
    target_link_options(nexus INTERFACE -fcontracts)
endif()

# 自有目标的编译面: 警告仅 PRIVATE(不波及 taskflow/doctest 等第三方头) +
# 契约求值语义(Debug 下 enforce, 其余 ignore; 不挂 INTERFACE, 不随之下发)
function(nexus_own_compile_options tgt)
    target_compile_options(
        ${tgt} PRIVATE -Wall -Wextra -Wshadow
        $<IF:$<CONFIG:Debug>,-fcontract-evaluation-semantic=enforce,-fcontract-evaluation-semantic=ignore>)
endfunction()

add_executable(nexus_example src/main.cpp)
target_link_libraries(nexus_example PRIVATE nexus)
nexus_own_compile_options(nexus_example)

add_executable(nexus_tests tests/main.cpp tests/pool.cpp tests/task.cpp tests/parallel.cpp
                                tests/detail.cpp)
target_link_libraries(nexus_tests PRIVATE nexus)
target_include_directories(nexus_tests PRIVATE tests tests/third_party)
nexus_own_compile_options(nexus_tests)
add_test(NAME nexus_tests COMMAND nexus_tests)

add_executable(nexus_bench benchmarks/bench.cpp)
target_link_libraries(nexus_bench PRIVATE nexus)
target_include_directories(nexus_bench PRIVATE benchmarks benchmarks/third_party/taskflow_repo)
nexus_own_compile_options(nexus_bench)

# oneTBB: 系统包引入(未随仓库分发), 未找到时跳过 TBB 对比行
find_package(TBB CONFIG QUIET)
if(TBB_FOUND)
    target_link_libraries(nexus_bench PRIVATE TBB::tbb)
    target_compile_definitions(nexus_bench PRIVATE NEXUS_BENCH_TBB=1)
endif()

# -DBUILD_MODULE=ON: module wrapper `huxint.nexus` (requires Ninja)
option(BUILD_MODULE "build the huxint.nexus module wrapper" OFF)
if(BUILD_MODULE)
    add_library(nexus_module STATIC)
    target_sources(nexus_module PUBLIC FILE_SET CXX_MODULES FILES module/nexus.cppm)
    target_link_libraries(nexus_module PUBLIC nexus)
    set_target_properties(nexus_module PROPERTIES CXX_SCAN_FOR_MODULES ON)
    nexus_own_compile_options(nexus_module)

    add_executable(nexus_module_smoke module/smoke.cpp)
    target_link_libraries(nexus_module_smoke PRIVATE nexus_module)
    set_target_properties(nexus_module_smoke PROPERTIES CXX_SCAN_FOR_MODULES ON)
    nexus_own_compile_options(nexus_module_smoke)
    add_test(NAME nexus_module_smoke COMMAND nexus_module_smoke)
endif()

# -DWITH_STDEXEC=ON -DSTDEXEC_ROOT=<含 stdexec/ 的目录>: P2300 scheduler 集成
# 可选依赖, 不进核心构建; 核心库保持零依赖 header-only
option(WITH_STDEXEC "build stdexec (P2300) scheduler integration tests" OFF)
if(WITH_STDEXEC)
    if(NOT STDEXEC_ROOT)
        message(FATAL_ERROR "WITH_STDEXEC=ON requires -DSTDEXEC_ROOT=<include root containing stdexec/>")
    endif()
    add_executable(nexus_stdexec_tests tests/stdexec.cpp)
    target_link_libraries(nexus_stdexec_tests PRIVATE nexus)
    # SYSTEM: 第三方头告警不属于本库承诺的零警告面
    target_include_directories(nexus_stdexec_tests SYSTEM PRIVATE ${STDEXEC_ROOT})
    target_include_directories(nexus_stdexec_tests PRIVATE tests tests/third_party)
    nexus_own_compile_options(nexus_stdexec_tests)
    # 关闭模块依赖扫描: 扫描注入的 -fmodules-ts 与 stdexec 头在 GCC 16 上
    # 触发编译器内部错误(本目标无模块单元, 扫描本无必要)
    set_target_properties(nexus_stdexec_tests PROPERTIES CXX_SCAN_FOR_MODULES OFF)
    add_test(NAME nexus_stdexec_tests COMMAND nexus_stdexec_tests)
endif()

# -DBUILD_CODSPEED_BENCH=ON: CodSpeed 回归基准(google_benchmark 兼容层, 网络拉取)
# 计量模式由 -DCODSPEED_MODE=simulation|walltime|memory 选择, 缺省 off 时退化为
# 普通 google_benchmark 可执行文件
option(BUILD_CODSPEED_BENCH "build the CodSpeed (google_benchmark) benchmarks" OFF)
if(BUILD_CODSPEED_BENCH)
    include(FetchContent)
    # instrument-hooks 的 dist/core.c 是 zig 转译产物, 其 [[noreturn]] 位置在
    # C23 下非法 -> 该依赖统一按 C17 编译(GCC 15+ 默认 C23)
    enable_language(C)
    set(CMAKE_C_STANDARD 17)
    set(CMAKE_C_STANDARD_REQUIRED ON)
    set(BENCHMARK_ENABLE_TESTING OFF)
    set(BENCHMARK_ENABLE_INSTALL OFF)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF)
    FetchContent_Declare(
        google_benchmark
        GIT_REPOSITORY https://github.com/CodSpeedHQ/codspeed-cpp
        GIT_TAG v2.4.0
        SOURCE_SUBDIR google_benchmark)
    FetchContent_MakeAvailable(google_benchmark)
    if(TARGET instrument_hooks)
        set_target_properties(instrument_hooks PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
    endif()

    add_executable(nexus_codspeed_bench benchmarks/codspeed_bench.cpp)
    target_link_libraries(nexus_codspeed_bench PRIVATE nexus benchmark::benchmark)
    nexus_own_compile_options(nexus_codspeed_bench)
endif()

if(SANITIZER STREQUAL "thread")
    set_tests_properties(nexus_tests PROPERTIES
        ENVIRONMENT "TSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/tests/tsan.supp")
endif()
