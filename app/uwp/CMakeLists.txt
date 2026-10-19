# Xbox (UWP) app. Built as a subdirectory of ModernGekko with MODERNGEKKO_UWP=ON
# (scripts/build_xbox.sh).
set(_imgui "${CMAKE_SOURCE_DIR}/vendor/dolphin/Externals/imgui/imgui")
add_executable(ColosseumXbox WIN32
  App.cpp
  disc_setup.cpp
  "${_imgui}/backends/imgui_impl_dx11.cpp")
target_include_directories(ColosseumXbox PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../launcher"
  "${CMAKE_SOURCE_DIR}/src/frontend"
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_SOURCE_DIR}/vendor/dolphin/Source/Core"
  "${_imgui}"
  "${_imgui}/backends")
target_compile_definitions(ColosseumXbox PRIVATE _M_X86_64=1 _ARCH_64=1 WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(ColosseumXbox PRIVATE ModernGekko::Runtime core uicommon imgui
  d3d11 d3d12 dxgi d3dcompiler runtimeobject)
target_link_options(ColosseumXbox PRIVATE -fexperimental-library -municode)
