# Build and start the native Linux service on one connected Arduino UNO Q over
# ADB. The image is built on the board's ARM64 Linux side from Dockerfile.native.
# Only a container this project started is replaced; the MCU is never flashed.
#
#   cmake -P cmake/DeployUnoQ.cmake [-DADB=/path/to/adb] [-DSERIAL=device-id]
#         [-DSTATE_FROM_REFERENCE=ON]
#
# Jobs live in ~/.local/share/open-braille-embosser-native on the board.
# STATE_FROM_REFERENCE=ON copies the queue of the earlier Python service,
# kept in ~/.local/share/open-braille-embosser, into that empty directory after
# the old container has stopped; the old directory itself is never modified.
cmake_minimum_required(VERSION 3.23)

get_filename_component(source "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(image open-braille-native:local)
set(container open-braille-service)

function(fail message)
    message(FATAL_ERROR "error: deployment failed: ${message}")
endfunction()

if(NOT ADB)
    find_program(ADB adb)
endif()
if(NOT ADB)
    # Arduino App Lab installs its own ADB under Arduino15.
    file(GLOB candidates
        "$ENV{HOME}/.arduino15/packages/arduino/tools/adb/*/adb"
        "$ENV{HOME}/Library/Arduino15/packages/arduino/tools/adb/*/adb"
        "$ENV{LOCALAPPDATA}/Arduino15/packages/arduino/tools/adb/*/adb.exe")
    if(candidates)
        list(SORT candidates COMPARE NATURAL)
        list(GET candidates -1 ADB)
    endif()
endif()
if(NOT ADB)
    fail("ADB is missing; install Arduino App Lab or pass -DADB=/path/to/adb")
endif()

execute_process(COMMAND "${ADB}" devices OUTPUT_VARIABLE listing RESULT_VARIABLE status)
if(status)
    fail("Cannot list ADB devices")
endif()
string(REPLACE "\r" "" listing "${listing}")
string(REGEX MATCHALL "[^\n]+" lines "${listing}")
set(devices)
foreach(line IN LISTS lines)
    if(line MATCHES "^([^\t ]+)[\t ]+device$")
        list(APPEND devices "${CMAKE_MATCH_1}")
    endif()
endforeach()
list(LENGTH devices count)
if(NOT SERIAL AND count EQUAL 1)
    list(GET devices 0 SERIAL)
endif()
if(NOT SERIAL OR NOT SERIAL IN_LIST devices)
    fail("Connect one ready UNO Q, or choose it with -DSERIAL=...; finish any board update first")
endif()

# Run one command in the board's Linux shell, streaming its output.
function(board command)
    execute_process(COMMAND "${ADB}" -s "${SERIAL}" shell "${command}" RESULT_VARIABLE status)
    if(status)
        fail("Board command failed (${status}): ${command}")
    endif()
endfunction()

# Run one command in the board's Linux shell and keep its standard output.
function(board_output variable command)
    execute_process(COMMAND "${ADB}" -s "${SERIAL}" shell "${command}" RESULT_VARIABLE status
        OUTPUT_VARIABLE output OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(status)
        fail("Board command failed (${status}): ${command}")
    endif()
    string(REPLACE "\r" "" output "${output}")
    set(${variable} "${output}" PARENT_SCOPE)
endfunction()

board_output(info [=[printf 'arch=%s\nuid=%s\nhome=%s\nhost=%s\n' "$(uname -m)" "$(id -u)" "$HOME" "$(hostname)"; busctl --system call org.freedesktop.Avahi / org.freedesktop.Avahi.Server GetHostNameFqdn 2>/dev/null || true]=])
foreach(key IN ITEMS arch uid home host)
    set(${key} "")
    if(info MATCHES "(^|\n)${key}=([^\n]*)")
        set(${key} "${CMAKE_MATCH_2}")
    endif()
endforeach()
set(advertised "${host}.local")
if(info MATCHES "\ns \"([^\"]+)\"")
    set(advertised "${CMAKE_MATCH_1}")
endif()
if(NOT arch STREQUAL "aarch64")
    fail("This image targets the UNO Q ARM64 Linux system, not ${arch}")
endif()
# Values below are interpolated into board shell commands.
if(NOT uid MATCHES "^[0-9]+$" OR NOT home MATCHES "^/[A-Za-z0-9._/-]+$" OR NOT host MATCHES "^[A-Za-z0-9.-]+$"
        OR NOT advertised MATCHES "^[A-Za-z0-9.-]+$")
    fail("Unexpected board account details:\n${info}")
endif()
board("docker version --format 'Docker {{.Server.Version}} on the board'")

set(context "${home}/open-braille-native")
set(state "${home}/.local/share/open-braille-embosser-native")
set(reference_state "${home}/.local/share/open-braille-embosser")

if(STATE_FROM_REFERENCE)
    board("if [ -e '${state}/queue.state' ]; then echo 'The native queue already exists; nothing will be copied' >&2; exit 1; fi; \
[ -e '${reference_state}/queue.state' ] || { echo 'No queue from the earlier Python service to copy' >&2; exit 1; }")
endif()

set(temporary "$ENV{TMPDIR}")
if(NOT temporary)
    set(temporary "$ENV{TEMP}")
endif()
if(NOT temporary)
    set(temporary "/tmp")
endif()
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef nonce)
set(work "${temporary}/open-braille-deploy-${nonce}")
file(MAKE_DIRECTORY "${work}")

# The files Dockerfile.native copies; tests and demo inputs stay local.
file(GLOB demo_sources RELATIVE "${source}" "${source}/demo/*.cpp" "${source}/demo/*.hpp")
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar czf "${work}/source.tar.gz" --
        Dockerfile.native .dockerignore CMakeLists.txt cmake/BuildPappl.cmake pappl_spool.patch
        spanish-numbers.cti README.md LICENSE src firmware ${demo_sources}
    WORKING_DIRECTORY "${source}" RESULT_VARIABLE status)
if(status)
    fail("Cannot package the native sources")
endif()
board_output(remote_archive "mktemp /tmp/open-braille-native-XXXXXXXX.tar.gz")
execute_process(COMMAND "${ADB}" -s "${SERIAL}" push "${work}/source.tar.gz" "${remote_archive}" RESULT_VARIABLE status)
if(status)
    fail("Cannot copy the sources to the board")
endif()
board("rm -rf '${context}.new' && mkdir -p '${context}.new' '${state}' && tar -xzf '${remote_archive}' -C '${context}.new' \
&& rm -f '${remote_archive}' && rm -rf '${context}' && mv '${context}.new' '${context}'")

message(STATUS "Building ${image} on the board; existing apps keep running.")
board("docker build --network=host -f '${context}/Dockerfile.native' -t ${image} '${context}'")
board_output(cells [=[docker run --rm --network none --entrypoint /bin/sh open-braille-native:local -c 'printf cat > /tmp/check.txt && /opt/open-braille/bin/braille /tmp/check.txt /tmp/check.brl && cat /tmp/check.brl']=])
if(NOT cells STREQUAL "⠀⠀⠉⠁⠞")
    fail("The built converter translated \"cat\" as \"${cells}\"")
endif()
message(STATUS "The converter in the new image produced the expected cells.")

board_output(existing "docker ps -aq --filter name=^${container}$")
if(existing)
    board_output(label "docker inspect --format '{{index .Config.Labels \"org.openbraille.service\"}}' ${container}")
    if(NOT label STREQUAL "1")
        fail("An unrelated container already uses the name ${container}; nothing was stopped")
    endif()
    board("docker stop --time 15 ${container} && docker rm ${container}")
endif()
if(STATE_FROM_REFERENCE)
    board("cp -a '${reference_state}/.' '${state}/'")
endif()

board("docker run -d --init --name ${container} --label org.openbraille.service=1 --restart unless-stopped \
--network host --hostname ${host} --user ${uid}:${uid} --memory 2g --pids-limit 128 --read-only \
--tmpfs /tmp:rw,nosuid,nodev,size=256m --cap-drop ALL --security-opt no-new-privileges \
--mount type=bind,source=${state},target=/data \
--mount type=bind,source=/run/dbus/system_bus_socket,target=/run/dbus/system_bus_socket,readonly \
${image} --state /data --bind 0.0.0.0 --hostname ${advertised}")

# Check the upload API from this computer through an ADB port forward.
execute_process(COMMAND "${ADB}" -s "${SERIAL}" forward tcp:0 tcp:8000
    OUTPUT_VARIABLE port OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE status)
if(status OR NOT port MATCHES "^[0-9]+$")
    fail("Cannot forward a local port to the board")
endif()
set(ready FALSE)
foreach(attempt RANGE 60)
    file(DOWNLOAD "http://127.0.0.1:${port}/api/capabilities" "${work}/capabilities.json" STATUS result TIMEOUT 3)
    list(GET result 0 code)
    if(code EQUAL 0)
        file(READ "${work}/capabilities.json" capabilities)
        if(capabilities MATCHES "\"printer_id\"")
            set(ready TRUE)
            break()
        endif()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1)
endforeach()
execute_process(COMMAND "${ADB}" -s "${SERIAL}" forward --remove "tcp:${port}")
file(REMOVE_RECURSE "${work}")
if(NOT ready)
    execute_process(COMMAND "${ADB}" -s "${SERIAL}" shell "docker logs --tail 40 ${container}")
    fail("Service did not become available")
endif()

message(STATUS "Upload/review: http://${advertised}:8000/")
message(STATUS "Printer URI: ipp://${advertised}:8631/ipp/print")
message(STATUS "Jobs prepare previews and remain held; no paper is embossed.")
