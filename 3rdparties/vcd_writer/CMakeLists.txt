# Small VCD waveform writer pulled in by base/recorder.cpp.
add_library(vcd_writer STATIC vcd_writer.cpp)
set_target_properties(vcd_writer PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(vcd_writer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
