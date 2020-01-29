add_executable(duckstation-sdl
  imgui_impl_sdl.cpp
  imgui_impl_sdl.h
  main.cpp
  opengl_host_display.cpp
  opengl_host_display.h
  sdl_host_interface.cpp
  sdl_host_interface.h
  sdl_settings_interface.cpp
  sdl_settings_interface.h
)

if(WIN32)
  target_sources(duckstation-sdl PRIVATE
    d3d11_host_display.cpp
    d3d11_host_display.h
  )
  target_link_libraries(duckstation-sdl PRIVATE d3d11.lib dxgi.lib)
endif()

target_link_libraries(duckstation-sdl PRIVATE core common imgui nativefiledialog glad simpleini SDL2::Main frontend-common)
