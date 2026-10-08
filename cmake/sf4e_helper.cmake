# The game and Sidecar remain x86. Cargo explicitly builds the shipped helper
# for x64 Windows, independent of the active CMake/MSVC architecture.
option(SF4E_BUILD_IROH_HELPER "Build the bundled x64 Iroh helper" ON)
if(WIN32)
    add_library(sf4e_helper_platform STATIC
        "${CMAKE_SOURCE_DIR}/src/platform/HelperProcess.cxx"
        "${CMAKE_SOURCE_DIR}/src/platform/HelperClient.cxx")
    target_compile_features(sf4e_helper_platform PUBLIC cxx_std_11)
    target_compile_definitions(sf4e_helper_platform PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(sf4e_helper_platform PUBLIC bcrypt)

    if(SF4E_BUILD_IROH_HELPER)
        find_program(SF4E_CARGO_EXECUTABLE cargo REQUIRED)
        set(SF4E_HELPER_SOURCE "${CMAKE_SOURCE_DIR}/rust/sf4-net")
        set(SF4E_HELPER_EXECUTABLE "${CMAKE_BINARY_DIR}/sf4-net.exe")
        file(GLOB_RECURSE SF4E_HELPER_RUST_SOURCES CONFIGURE_DEPENDS "${SF4E_HELPER_SOURCE}/src/*.rs"
            "${CMAKE_SOURCE_DIR}/server/ember/protocol/src/*.rs")
        set(SF4E_HELPER_INPUTS ${SF4E_HELPER_RUST_SOURCES} "${SF4E_HELPER_SOURCE}/Cargo.toml"
            "${SF4E_HELPER_SOURCE}/Cargo.lock" "${SF4E_HELPER_SOURCE}/rust-toolchain.toml"
            "${CMAKE_SOURCE_DIR}/server/ember/protocol/Cargo.toml" "${CMAKE_SOURCE_DIR}/server/ember/Cargo.toml"
            "${SF4E_HELPER_SOURCE}/build.rs" "${SF4E_HELPER_SOURCE}/resource.rc" "${CMAKE_SOURCE_DIR}/src/ui/ember.ico")
        set(SF4E_HELPER_RUSTFLAGS "-C target-feature=+crt-static")
        set(SF4E_HELPER_CARGO_ARGS build --locked --release --target x86_64-pc-windows-msvc)
        set(SF4E_HELPER_CARGO_OUTPUT "${CMAKE_BINARY_DIR}/rust-target/x86_64-pc-windows-msvc/release/sf4-net.exe")
        # Most branches never touch Rust, and the helper is most of a cold
        # build. A test or CI build may name a registry to fetch the helper
        # from when an earlier build made it from these same inputs and flags.
        # Empty builds it here, which is what a release build must do: its
        # receipt lists hashes of files it compiled itself.
        set(SF4E_PREBUILT_REGISTRY "" CACHE STRING
            "Registry path holding prebuilt parts for test builds, such as harbor.fraccie.be/sf4ember/prebuilt")
        set(SF4E_HELPER_BUILD ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/rust-target"
            "CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_RUSTFLAGS=${SF4E_HELPER_RUSTFLAGS}"
            "${SF4E_CARGO_EXECUTABLE}" ${SF4E_HELPER_CARGO_ARGS})
        if(SF4E_PREBUILT_REGISTRY)
            string(REPLACE "${CMAKE_SOURCE_DIR}/" "" SF4E_HELPER_INPUT_LIST "${SF4E_HELPER_INPUTS}")
            string(REPLACE ";" "\n" SF4E_HELPER_INPUT_LIST "${SF4E_HELPER_INPUT_LIST}")
            file(WRITE "${CMAKE_BINARY_DIR}/prebuilt/sf4-net.inputs" "${SF4E_HELPER_INPUT_LIST}\n")
            # PowerShell would read the build command's own options as its
            # parameters, so the command travels as one value.
            list(JOIN SF4E_HELPER_BUILD "|" SF4E_HELPER_BUILD_VALUE)
            list(JOIN SF4E_HELPER_CARGO_ARGS " " SF4E_HELPER_VARIANT)
            set(SF4E_HELPER_BUILD powershell.exe -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/scripts/use-prebuilt.ps1" -Repository "${SF4E_PREBUILT_REGISTRY}/sf4-net"
                -Output "${SF4E_HELPER_CARGO_OUTPUT}" -SourceRoot "${CMAKE_SOURCE_DIR}"
                -InputList "${CMAKE_BINARY_DIR}/prebuilt/sf4-net.inputs"
                -Variant "${SF4E_HELPER_VARIANT} rustflags=${SF4E_HELPER_RUSTFLAGS}"
                -BuildCommand "${SF4E_HELPER_BUILD_VALUE}")
        endif()
        add_custom_command(OUTPUT "${SF4E_HELPER_EXECUTABLE}"
            COMMAND ${SF4E_HELPER_BUILD}
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SF4E_HELPER_CARGO_OUTPUT}" "${SF4E_HELPER_EXECUTABLE}"
            WORKING_DIRECTORY "${SF4E_HELPER_SOURCE}"
            DEPENDS ${SF4E_HELPER_INPUTS}
            VERBATIM COMMENT "Build the pinned x64 Iroh helper")
        add_custom_target(IrohHelper ALL DEPENDS "${SF4E_HELPER_EXECUTABLE}")
        install(PROGRAMS "${SF4E_HELPER_EXECUTABLE}" DESTINATION ".")
        if(BUILD_TESTING)
            add_executable(HelperProcessTest "${CMAKE_SOURCE_DIR}/src/tests/helper_process_test.cxx")
            target_link_libraries(HelperProcessTest PRIVATE sf4e_helper_platform user32)
            add_dependencies(HelperProcessTest IrohHelper)
            add_test(NAME HelperProcess COMMAND HelperProcessTest "${SF4E_HELPER_EXECUTABLE}")
            set_tests_properties(HelperProcess PROPERTIES TIMEOUT 90)
            sf4e_test_locks(HelperProcess loopback_udp_ports ember_processes)
            add_test(NAME PrebuiltKeyPowerShell COMMAND powershell.exe -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/scripts/test-prebuilt-key.ps1")
            set_tests_properties(PrebuiltKeyPowerShell PROPERTIES TIMEOUT 60)
            # The helper's own tests belong to the same gate as the C++ tests,
            # so a build receipt with testsPassed covers them (ledger A-015).
            # A separate target dir keeps the release helper from rebuilding.
            # When the sources changed, Cargo compiles the tests on
            # SF4E_CARGO_TEST_JOBS processors, the number CTest counts for the
            # test. The tests themselves mostly wait on timers, so they keep
            # one thread per processor, as Cargo runs them by default.
            set(SF4E_CARGO_TEST_JOBS 2 CACHE STRING "Concurrent Cargo jobs per registered Rust test suite")
            if(NOT SF4E_CARGO_TEST_JOBS MATCHES "^[1-9][0-9]*$")
                message(FATAL_ERROR "SF4E_CARGO_TEST_JOBS must be a positive integer")
            endif()
            add_test(NAME HelperRust
                COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${CMAKE_BINARY_DIR}/rust-test-target"
                    "CARGO_TARGET_X86_64_PC_WINDOWS_MSVC_RUSTFLAGS=-C target-feature=+crt-static"
                    "${SF4E_CARGO_EXECUTABLE}" test --locked -j ${SF4E_CARGO_TEST_JOBS} --target x86_64-pc-windows-msvc
                WORKING_DIRECTORY "${SF4E_HELPER_SOURCE}")
            set_tests_properties(HelperRust PROPERTIES TIMEOUT 1800 PROCESSORS ${SF4E_CARGO_TEST_JOBS})
            sf4e_test_locks(HelperRust loopback_udp_ports ember_processes)
            # The identity protocol and tournament bridge workspace. The bridge
            # is not shipped to players; its tests still gate the build. The SDK
            # test builds into the same target directory and runs that bridge.
            set(SF4E_EMBER_TARGET_DIR "${CMAKE_BINARY_DIR}/rust-ember-target")
            add_test(NAME EmberRust
                COMMAND ${CMAKE_COMMAND} -E env "CARGO_TARGET_DIR=${SF4E_EMBER_TARGET_DIR}"
                    "${SF4E_CARGO_EXECUTABLE}" test --locked -j ${SF4E_CARGO_TEST_JOBS}
                WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/server/ember")
            set_tests_properties(EmberRust PROPERTIES TIMEOUT 1800 PROCESSORS ${SF4E_CARGO_TEST_JOBS})
            sf4e_test_locks(EmberRust ember_processes)
            find_program(SF4E_NPM_EXECUTABLE NAMES npm.cmd npm REQUIRED)
            add_test(NAME BridgeSdk
                COMMAND ${CMAKE_COMMAND} "-DSOURCE=${CMAKE_SOURCE_DIR}"
                    "-DCARGO=${SF4E_CARGO_EXECUTABLE}" "-DNPM=${SF4E_NPM_EXECUTABLE}"
                    "-DEMBER_TARGET=${SF4E_EMBER_TARGET_DIR}" "-DJOBS=${SF4E_CARGO_TEST_JOBS}"
                    -P "${CMAKE_SOURCE_DIR}/cmake/RunSdkTests.cmake")
            set_tests_properties(BridgeSdk PROPERTIES TIMEOUT 1800 PROCESSORS ${SF4E_CARGO_TEST_JOBS})
            sf4e_test_locks(BridgeSdk ember_processes)
        endif()
    endif()
endif()
