# CMake generated Testfile for 
# Source directory: /Users/tianyunzhang/Documents/Columbia/Programming for FE/project2/hft-cpp-columbia/hft/cpp_client
# Build directory: /Users/tianyunzhang/Documents/Columbia/Programming for FE/project2/hft-cpp-columbia/hft/cpp_client/build-tsan
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(bot_pipeline "/Users/tianyunzhang/Documents/Columbia/Programming for FE/project2/hft-cpp-columbia/hft/cpp_client/build-tsan/bot_pipeline_test")
set_tests_properties(bot_pipeline PROPERTIES  TIMEOUT "30" _BACKTRACE_TRIPLES "/Users/tianyunzhang/Documents/Columbia/Programming for FE/project2/hft-cpp-columbia/hft/cpp_client/CMakeLists.txt;111;add_test;/Users/tianyunzhang/Documents/Columbia/Programming for FE/project2/hft-cpp-columbia/hft/cpp_client/CMakeLists.txt;0;")
subdirs("_deps/nlohmann_json-build")
subdirs("_deps/ixwebsocket-build")
