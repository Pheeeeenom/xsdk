# Read after CMake's compiler modules, for each language xsdk.cmake builds.
# Titles link whole at a fixed address (src/xsdk.ld), so code a project asks to be position independent isn't.
set(CMAKE_C_COMPILE_OPTIONS_PIC "")
set(CMAKE_C_COMPILE_OPTIONS_PIE "")
set(CMAKE_CXX_COMPILE_OPTIONS_PIC "")
set(CMAKE_CXX_COMPILE_OPTIONS_PIE "")
