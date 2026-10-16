add_executable(TaskSyncTest
  TaskSyncTest.cpp
  ${REPO_ROOT}/lib/TaskSync/TwCrypto.cpp
  ${REPO_ROOT}/lib/TaskSync/TaskModel.cpp
  ${REPO_ROOT}/lib/TaskSync/TaskSyncEngine.cpp
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp
  ${REPO_ROOT}/lib/miniz/src/InflateStream.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
  ${REPO_ROOT}/lib/hal/HalMemory.cpp
)

target_include_directories(TaskSyncTest PRIVATE
  ${REPO_ROOT}/test/inflate_stream/stubs
  ${REPO_ROOT}/lib/TaskSync
  ${REPO_ROOT}/lib/miniz/src
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/include
)

target_link_libraries(TaskSyncTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TaskSyncTest)
