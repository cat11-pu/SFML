add_executable(sfml-window-context-rebuild-stress
    WindowContextRebuildStress.cpp
)

target_compile_features(sfml-window-context-rebuild-stress PRIVATE cxx_std_17)
target_link_libraries(sfml-window-context-rebuild-stress PRIVATE SFML::Graphics)
set_target_warnings(sfml-window-context-rebuild-stress)

if(SFML_OS_LINUX OR SFML_OS_FREEBSD OR SFML_OS_OPENBSD OR SFML_OS_NETBSD)
    find_package(X11 REQUIRED)
    target_link_libraries(sfml-window-context-rebuild-stress PRIVATE X11::X11)
    target_compile_definitions(sfml-window-context-rebuild-stress PRIVATE SFML_STRESS_HAS_X11)
endif()

add_test(NAME window-context-rebuild-stress
         COMMAND sfml-window-context-rebuild-stress --iterations 200)
