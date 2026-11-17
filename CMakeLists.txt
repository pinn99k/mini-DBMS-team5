cmake_minimum_required(VERSION 3.20)
project(minidb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(MINIDB_SANITIZE "" CACHE STRING "Sanitizer: address (ASan+UBSan) | thread | empty")
set(MINIDB_TEST_LAUNCHER "")

add_compile_options(-Wall -Wextra -Werror)
if(MINIDB_SANITIZE STREQUAL "address")
  # -O1: ASan 권장. -O0이면 100만 건 테스트가 수십 배 느리다
  add_compile_options(-O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
elseif(MINIDB_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
  # WSL2 커널(mmap_rnd_bits=32)에서 TSan은 ASLR을 꺼야 시작된다 (decisions #7)
  set(MINIDB_TEST_LAUNCHER setarch ${CMAKE_HOST_SYSTEM_PROCESSOR} -R)
endif()

find_package(Threads REQUIRED)
find_package(GTest REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(HTTPLIB REQUIRED IMPORTED_TARGET cpp-httplib)

# ---- 모듈 (의존 방향: cli → server → engine → sql / storage → index → common)
add_library(minidb_common STATIC src/common/crc32.cpp src/common/json_writer.cpp)
target_include_directories(minidb_common PUBLIC src)

add_library(minidb_index INTERFACE)
target_link_libraries(minidb_index INTERFACE minidb_common)

set(MINIDB_STORAGE_SOURCES
  src/storage/schema.cpp src/storage/file_io.cpp src/storage/frame_codec.cpp src/storage/frame_scan.cpp
  src/storage/table.cpp src/storage/database.cpp)
add_library(minidb_storage STATIC ${MINIDB_STORAGE_SOURCES})
target_link_libraries(minidb_storage PUBLIC minidb_common minidb_index Threads::Threads)

add_library(minidb_sql STATIC src/sql/lexer.cpp src/sql/parser.cpp)
target_link_libraries(minidb_sql PUBLIC minidb_common)

add_library(minidb_engine STATIC src/engine/binder.cpp src/engine/planner.cpp src/engine/executor.cpp)
target_link_libraries(minidb_engine PUBLIC minidb_sql minidb_storage)

add_library(minidb_server STATIC
  src/server/thread_pool.cpp src/server/http_server.cpp src/server/query_handler.cpp
  src/server/ndjson_sink.cpp src/server/serve.cpp)
target_link_libraries(minidb_server PUBLIC minidb_engine PkgConfig::HTTPLIB)

add_executable(minidb src/cli/main.cpp src/cli/table_printer.cpp src/cli/dump.cpp)
target_link_libraries(minidb PRIVATE minidb_server)

# ---- 벤치마크
add_executable(minidb_bench bench/bench_index.cpp)
target_link_libraries(minidb_bench PRIVATE minidb_engine)

add_executable(minidb_bench_lock bench/bench_lock.cpp)
target_link_libraries(minidb_bench_lock PRIVATE minidb_storage)
# 같은 벤치를 "읽기도 unique_lock"으로 빌드해 shared_mutex와 비교한다
add_library(minidb_storage_exclusive STATIC ${MINIDB_STORAGE_SOURCES})
target_compile_definitions(minidb_storage_exclusive PUBLIC MINIDB_EXCLUSIVE_LOCK)
target_link_libraries(minidb_storage_exclusive PUBLIC minidb_common minidb_index Threads::Threads)
add_executable(minidb_bench_lock_exclusive bench/bench_lock.cpp)
target_link_libraries(minidb_bench_lock_exclusive PRIVATE minidb_storage_exclusive)

# ---- 테스트
enable_testing()
function(minidb_test name)
  add_executable(${name} tests/${name}.cpp)
  target_include_directories(${name} PRIVATE tests)
  target_link_libraries(${name} PRIVATE ${ARGN} GTest::gtest_main)
  add_test(NAME ${name} COMMAND ${MINIDB_TEST_LAUNCHER} $<TARGET_FILE:${name}>)
endfunction()

minidb_test(common_test minidb_common)
minidb_test(storage_test minidb_storage)
minidb_test(index_test minidb_index)
minidb_test(sql_test minidb_sql)
minidb_test(engine_test minidb_engine)
minidb_test(concurrency_test minidb_server)
minidb_test(server_test minidb_server)
set_tests_properties(concurrency_test server_test PROPERTIES TIMEOUT 300)

add_test(NAME cli_e2e COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/cli_e2e.sh $<TARGET_FILE:minidb>)
add_test(NAME serve_e2e COMMAND bash ${CMAKE_SOURCE_DIR}/tests/e2e/serve_e2e.sh $<TARGET_FILE:minidb>)
string(REPLACE ";" " " MINIDB_LAUNCHER_STR "${MINIDB_TEST_LAUNCHER}")
set_tests_properties(cli_e2e serve_e2e PROPERTIES ENVIRONMENT "MINIDB_LAUNCHER=${MINIDB_LAUNCHER_STR}" TIMEOUT 120)
