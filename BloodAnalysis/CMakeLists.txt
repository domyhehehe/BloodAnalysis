cmake_minimum_required(VERSION 3.20)
project(BloodAnalysis)

set(CMAKE_CXX_STANDARD 17)

add_executable(BloodAnalysis
    BloodAnalysis.cpp
)

target_include_directories(BloodAnalysis PRIVATE
    .
)

# RocksDB を使う場合（vcpkgインストール済み前提）
find_package(RocksDB CONFIG REQUIRED)
target_link_libraries(BloodAnalysis PRIVATE RocksDB::rocksdb)
