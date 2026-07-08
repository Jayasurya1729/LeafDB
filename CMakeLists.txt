cmake_minimum_required(VERSION 3.14)
project(coredb CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(coredb
    main.cpp
    sql.cpp
    executor.cpp
    catalog.cpp
    storage.cpp
    wal.cpp
    kvstore.cpp
    btree.cpp
    lock_manager.cpp
    table.cpp
    record.cpp
    encode.cpp
    index.cpp
    ai_interface.cpp
    mcp_tools.cpp
    llm_adapter.cpp
    web_ui.cpp
)

find_package(CURL REQUIRED)

target_include_directories(coredb PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(coredb PRIVATE CURL::libcurl)