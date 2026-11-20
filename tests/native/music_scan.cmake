if("$ENV{BRAILLE_OMR_MODELS}" STREQUAL "")
    message(STATUS "BRAILLE_OMR_MODELS is unset")
    return()
endif()

string(RANDOM LENGTH 12 nonce)
set(work "${WORK}/music-scan-${nonce}")
file(MAKE_DIRECTORY "${work}")

foreach(extension IN ITEMS png pdf)
    set(score "${FIXTURES}/frere-jacques.${extension}")
    set(cells "${work}/${extension}.brl")
    set(review "${work}/${extension}-review")
    execute_process(COMMAND "${CLI}" "${score}" "${cells}" --music --music-review "${review}"
        --text-output "${work}/${extension}.txt" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files
        "${cells}" "${FIXTURES}/frere-jacques.expected.brl" COMMAND_ERROR_IS_FATAL ANY)

    set(xml "${review}/page-001.musicxml")
    execute_process(COMMAND "${CLI}" "${xml}" "${work}/from-xml.brl" COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files
        "${cells}" "${work}/from-xml.brl" COMMAND_ERROR_IS_FATAL ANY)

    file(SHA256 "${xml}" previous)
    execute_process(COMMAND "${CLI}" "${score}" "${cells}" --music --music-review "${review}"
        RESULT_VARIABLE status ERROR_VARIABLE error)
    file(SHA256 "${xml}" current)
    if(NOT status STREQUAL "2" OR NOT error MATCHES "review path already exists" OR NOT previous STREQUAL current)
        message(FATAL_ERROR "MusicXML review overwrite test failed: ${error}")
    endif()
endforeach()

file(REMOVE_RECURSE "${work}")
