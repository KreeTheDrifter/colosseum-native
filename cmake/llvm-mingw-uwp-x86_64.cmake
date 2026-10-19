# x86-64 UWP (Xbox Dev Mode) with llvm-mingw. Compiles with desktop API headers but links
# against windowsapp/ucrtapp only, so any API the Xbox doesn't allow fails at link time.
set(LLVM_MINGW "$ENV{LLVM_MINGW}")
set(CMAKE_SYSTEM_NAME Windows)
# Never pick up the build machine's Linux libraries (also on automatic re-configures).
set(ENV{PKG_CONFIG_LIBDIR} /nonexistent)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32uwp-clang)
set(CMAKE_CXX_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32uwp-clang++)
set(CMAKE_RC_COMPILER ${LLVM_MINGW}/bin/x86_64-w64-mingw32uwp-windres)
set(CMAKE_AR ${LLVM_MINGW}/bin/llvm-ar)
set(CMAKE_RANLIB ${LLVM_MINGW}/bin/llvm-ranlib)
set(CMAKE_FIND_ROOT_PATH ${LLVM_MINGW}/x86_64-w64-mingw32 ${LLVM_MINGW}/generic-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(WINFLAGS "-D____FIReference_1_boolean_INTERFACE_DEFINED__ -D_UNICODE -UWINAPI_FAMILY -DWINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DNTDDI_VERSION=0x0A00000C -idirafter $ENV{CASE_SHIM}")
set(CMAKE_C_FLAGS_INIT "${WINFLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${WINFLAGS}")
# windowsapp first, so functions resolve to the API sets UWP apps may import rather than to
# kernel32/advapi32/user32, and none of CMake's default desktop libraries.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -Wl,-lwindowsapp")
set(CMAKE_C_STANDARD_LIBRARIES "" CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_LIBRARIES "" CACHE STRING "" FORCE)
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-static -Wl,-lwindowsapp")
