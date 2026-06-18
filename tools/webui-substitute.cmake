# Two-stage WebUI preprocessing: decompress, substitute @DEFAULTFORMAT@, recompress.
# Usage: cmake -DINPUT=... -DOUTPUT=... -DDEFAULT_FORMAT=... -DGZIP_EXECUTABLE=... -DSED_EXECUTABLE=... -P webui-substitute.cmake

if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED DEFAULT_FORMAT OR NOT DEFINED GZIP_EXECUTABLE OR NOT DEFINED SED_EXECUTABLE)
    message(FATAL_ERROR "webui-substitute.cmake requires INPUT, OUTPUT, DEFAULT_FORMAT, GZIP_EXECUTABLE, and SED_EXECUTABLE.")
endif()

get_filename_component(OUTPUT_DIR "${OUTPUT}" DIRECTORY)
set(TEMP_HTML "${OUTPUT_DIR}/_webui_tmp.html")
set(TEMP_GZ   "${OUTPUT_DIR}/_webui_tmp_repl.html.gz")

# Decompress input
execute_process(
    COMMAND "${GZIP_EXECUTABLE}" -d -c "${INPUT}"
    OUTPUT_FILE "${TEMP_HTML}"
    RESULT_VARIABLE _rc
)
if(NOT _rc EQUAL 0)
    file(REMOVE "${TEMP_HTML}")
    message(FATAL_ERROR "Failed to decompress ${INPUT} (gzip -d -c returned ${_rc}).")
endif()

# Substitute placeholder
execute_process(
    COMMAND "${SED_EXECUTABLE}" "s|@DEFAULTFORMAT@|${DEFAULT_FORMAT}|g" "${TEMP_HTML}"
    OUTPUT_FILE "${TEMP_GZ}"
    RESULT_VARIABLE _rc
)
if(NOT _rc EQUAL 0)
    file(REMOVE "${TEMP_HTML}" "${TEMP_GZ}")
    message(FATAL_ERROR "Failed to substitute @DEFAULTFORMAT@ in ${TEMP_HTML} (sed returned ${_rc}).")
endif()

# Recompress to output
execute_process(
    COMMAND "${GZIP_EXECUTABLE}" -9 -n -c "${TEMP_GZ}"
    OUTPUT_FILE "${OUTPUT}"
    RESULT_VARIABLE _rc
)
if(NOT _rc EQUAL 0)
    file(REMOVE "${TEMP_HTML}" "${TEMP_GZ}")
    message(FATAL_ERROR "Failed to compress to ${OUTPUT} (gzip -9 -n -c returned ${_rc}).")
endif()

# Clean up temporaries
file(REMOVE "${TEMP_HTML}" "${TEMP_GZ}")
