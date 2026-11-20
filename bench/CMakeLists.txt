find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Network)
find_package(nlohmann_json 3.10 REQUIRED)

add_library(bench_window STATIC router.cpp window.cpp)
target_include_directories(bench_window PUBLIC "." "${PROJECT_SOURCE_DIR}/firmware/uno_q" "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(bench_window PUBLIC Qt6::Widgets Qt6::Network nlohmann_json::nlohmann_json)

qt_add_executable(open-braille-bench WIN32 MACOSX_BUNDLE main.cpp)
target_link_libraries(open-braille-bench PRIVATE bench_window)
set_target_properties(open-braille-bench PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.openbraille.bench"
    MACOSX_BUNDLE_BUNDLE_NAME "Open Braille Bench"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}")

install(TARGETS open-braille-bench
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT Bench
    BUNDLE DESTINATION . COMPONENT Bench)
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION "${CMAKE_INSTALL_DATADIR}/open-braille-bench" COMPONENT Bench)

if(APPLE OR WIN32)
    if(Qt6_VERSION VERSION_LESS 6.5)
        qt_generate_deploy_app_script(TARGET open-braille-bench FILENAME_VARIABLE deploy_bench)
    else()
        qt_generate_deploy_app_script(TARGET open-braille-bench OUTPUT_SCRIPT deploy_bench)
    endif()
    install(SCRIPT "${deploy_bench}" COMPONENT Bench)
endif()

if(BUILD_TESTING)
    find_package(Qt6 6.4 REQUIRED COMPONENTS Test)
    add_executable(test_bench "${PROJECT_SOURCE_DIR}/tests/native/bench.cpp")
    set_target_properties(test_bench PROPERTIES AUTOMOC ON)
    target_link_libraries(test_bench PRIVATE bench_window Qt6::Test)
    add_test(NAME bench-desktop COMMAND test_bench)
    set_tests_properties(bench-desktop PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 30)
endif()
