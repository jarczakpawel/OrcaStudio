add_library(slicer_linux_runtime_core STATIC
    RuntimeCoreFrame.cpp
    RuntimeCoreCallbackQueue.cpp
    RuntimeCoreMethodManifest.cpp
    RuntimeCoreSubprocess.cpp
)

target_include_directories(slicer_linux_runtime_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(slicer_linux_runtime_core PUBLIC nlohmann_json::nlohmann_json)
