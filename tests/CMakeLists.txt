# Suites include the implementation they intercept. Do not link those same
# translation units a second time; only add their remaining dependencies.
# Keep assert() active in optimized builds. Scope this to the test directory
# and avoid MSVC's D9025 warning from defining and undefining NDEBUG together.
foreach(config RELEASE RELWITHDEBINFO MINSIZEREL)
    string(REGEX REPLACE "(^| )[/-]DNDEBUG($| )" " "
        CMAKE_C_FLAGS_${config} "${CMAKE_C_FLAGS_${config}}")
endforeach()

function(taskman_add_test name)
    add_executable(${name} "${name}.c" ${ARGN})
    target_link_libraries(${name} PRIVATE taskman_platform)
    if(MSVC)
        target_compile_options(${name} PRIVATE "$<$<COMPILE_LANGUAGE:C>:/W4;/WX;/UNDEBUG>")
    else()
        target_compile_options(${name} PRIVATE "$<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Werror;-UNDEBUG>")
    endif()
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        WORKING_DIRECTORY "${PROJECT_BINARY_DIR}" TIMEOUT 60)
endfunction()

# Existing CSV and rendering fixtures use tests/.build relative to their CWD.
# Run in the binary tree so even these artifacts stay out of the source tree.
file(MAKE_DIRECTORY "${PROJECT_BINARY_DIR}/tests/.build")

taskman_add_test(test_settings)
taskman_add_test(test_launch
    ../src/ui.c ../src/settings.c ../src/sysinfo.c
    ../src/tabs/tab_apps.c ../src/tabs/tab_processes.c
    ../src/tabs/tab_network.c ../src/tabs/tab_users.c ../src/tabs/proc_tree.c)
taskman_add_test(test_collector)
taskman_add_test(test_sysinfo_live ../src/sysinfo.c)
taskman_add_test(test_processes ../src/tabs/proc_tree.c)
taskman_add_test(test_apps)
taskman_add_test(test_network)
taskman_add_test(test_services_users)
taskman_add_test(test_proctree)

set(workspace_sources)
foreach(source IN LISTS TASKMAN_SUPPORT_SOURCES TASKMAN_PAGE_SOURCES)
    list(APPEND workspace_sources "${PROJECT_SOURCE_DIR}/${source}")
endforeach()
taskman_add_test(test_workspace ${workspace_sources} "${TASKMAN_RESOURCE}")
# Source properties are directory-scoped; repeat the asset dependencies here.
set_property(SOURCE "${TASKMAN_RESOURCE}" PROPERTY OBJECT_DEPENDS
    "${TASKMAN_RESOURCE_DEPENDENCIES}")
if(MSVC)
    # This console fixture embeds the application's manifest through the RC.
    target_link_options(test_workspace PRIVATE /MANIFEST:NO)
endif()
set_tests_properties(test_workspace PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
