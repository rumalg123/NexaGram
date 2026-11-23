add_executable(Packer)
init_target(Packer)
add_dependencies(Telegram Packer)

target_include_directories(Packer PRIVATE ${src_loc})
nice_target_sources(Packer ${src_loc}
PRIVATE
    _other/packer.cpp
    _other/packer.h
    core/update_verify.cpp
    core/update_verify.h
)
target_compile_definitions(Packer PRIVATE PACKER_DISABLE_PRIVATE)
target_link_libraries(Packer
PRIVATE
    desktop-app::external_qt
    desktop-app::external_zlib
    desktop-app::external_auto_updates
    desktop-app::external_openssl
)
if (DESKTOP_APP_USE_PACKAGED)
    target_compile_definitions(Packer PRIVATE PACKER_USE_PACKAGED)
endif()
if (WIN32 AND DESKTOP_APP_WIN7_BASELINE)
    target_link_libraries(Packer PRIVATE desktop-app::lib_base)
    target_link_options(Packer PRIVATE /SUBSYSTEM:CONSOLE,6.01)
endif()
set_target_properties(Packer PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${output_folder})
