cmake_minimum_required(VERSION 3.20)
project(tispeech_native C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
enable_testing()

# Extracted speech data is ordinary C source. Normal builds need neither the
# original DLLs nor Python. Disable this option for a data-free library or to
# regenerate in the build tree from explicitly supplied DLLs.
option(TISPEECH_USE_EXTRACTED_DATA "Compile the bundled English/Spanish speech data" ON)
set(TISPEECH_ENG_DLL "" CACHE FILEPATH "Optional original TIENG32.DLL for extraction/verification")
set(TISPEECH_SPAN_DLL "" CACHE FILEPATH "Optional original TISPAN32.DLL for extraction/verification")
set(TISPEECH_BASE_DLL "" CACHE FILEPATH "Optional original TIBASE32.DLL for extraction/verification")
option(TISPEECH_VERIFY_ORIGINAL "Enable development-only Unicorn differential tests" OFF)
set(TISPEECH_VERIFY_CASES "20000" CACHE STRING "Cases per original-DLL differential test")

if((NOT TISPEECH_USE_EXTRACTED_DATA AND
    (TISPEECH_ENG_DLL OR TISPEECH_SPAN_DLL OR TISPEECH_BASE_DLL)) OR TISPEECH_VERIFY_ORIGINAL)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
endif()
foreach(language BASE ENG SPAN)
    set(_have_${language} OFF)
    if(TISPEECH_USE_EXTRACTED_DATA OR TISPEECH_${language}_DLL)
        set(_have_${language} ON)
    endif()
endforeach()

# Keep the original extractors available for comparisons and alternate inputs.
function(tispeech_data variable filename extractor)
    if(TISPEECH_USE_EXTRACTED_DATA)
        set(output "${CMAKE_CURRENT_SOURCE_DIR}/data/${filename}")
        if(NOT EXISTS "${output}")
            message(FATAL_ERROR "Missing extracted speech data: ${output}. Restore data/ or run tools/extract_data.py.")
        endif()
    else()
        set(output "${CMAKE_CURRENT_BINARY_DIR}/${filename}")
        set(dependencies)
        foreach(dll BASE ENG SPAN)
            if(TISPEECH_${dll}_DLL)
                list(APPEND dependencies "${TISPEECH_${dll}_DLL}")
            endif()
        endforeach()
        add_custom_command(OUTPUT "${output}"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/${extractor}"
                    ${ARGN}
            DEPENDS ${dependencies}
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/${extractor}"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/extract_generator.py"
            VERBATIM
            COMMENT "Extracting ${filename}")
    endif()
    set(${variable} "${output}" PARENT_SCOPE)
endfunction()

add_library(tispeech_rules STATIC src/ruleset.c)
target_include_directories(tispeech_rules PUBLIC include)

# ---------------------------------------------------------------------------
# Frame-to-PCM renderer and fixed-point sample kernel. Neither requires
# proprietary tables to compile; callers supply the data at the C boundary.
# ---------------------------------------------------------------------------
add_library(tispeech_dsp STATIC src/dsp.c)
target_include_directories(tispeech_dsp PUBLIC include)

add_library(tispeech_frames STATIC src/frames.c)
target_link_libraries(tispeech_frames PUBLIC tispeech_dsp)

# ---------------------------------------------------------------------------
# Pitch smoothing (TIBASE32 0x1c00cd40) and the two reconstructed generator
# leaves (TIENG32 0x1c203010 and 0x1c203870). Same contract as the renderer:
# both compile with no proprietary data, because the caller hands the tables
# in at the C boundary. Only the extracted table library below is gated.
# ---------------------------------------------------------------------------
add_library(tispeech_smoothing STATIC src/smoothing.c)
target_link_libraries(tispeech_smoothing PUBLIC tispeech_frames)

add_library(tispeech_generator STATIC src/generator.c)
target_include_directories(tispeech_generator PUBLIC include)

# ---------------------------------------------------------------------------
# The narrate pipeline: phoneme string -> records -> prosody -> parameter
# frames (TIBASE32 FUN_1c003870 and the language module's +0x04/+0x08), plus
# the voice-expression pass. Compiles without proprietary data; the tables
# are supplied as extracted images (bundled by default).
# ---------------------------------------------------------------------------
add_library(tispeech_userdict STATIC src/userdict.c)
target_include_directories(tispeech_userdict PUBLIC include)

add_library(tispeech_narrate STATIC
    src/narrate.c src/duration_eng.c src/duration_span.c src/langgen.c src/engine.c src/expression.c
    src/textphon_eng.c src/textphon_span.c src/dllload.c)
target_link_libraries(tispeech_narrate PUBLIC tispeech_smoothing tispeech_frames tispeech_rules tispeech_userdict)

# Both test binaries run without any DLL: their fixtures are synthetic. The
# English dataset adds the real-inventory cases inside test_generator.c.
add_executable(test_smoothing tests/test_smoothing.c)
target_link_libraries(test_smoothing PRIVATE tispeech_smoothing)
add_test(NAME smoothing COMMAND test_smoothing)

# Linked below against tispeech_gen_eng when the English tables are available,
# which carries tispeech_generator with it; against the bare stage otherwise.
add_executable(test_generator tests/test_generator.c)
add_test(NAME generator COMMAND test_generator)

# Also DLL-free: hand-built dictionary bytes, no extraction needed.
add_executable(test_userdict tests/test_userdict.c)
target_link_libraries(test_userdict PRIVATE tispeech_userdict)
add_test(NAME userdict COMMAND test_userdict)

# ---------------------------------------------------------------------------
# Base synthesis coefficients, waveforms, phoneme names and voice tables.
# ---------------------------------------------------------------------------
if(_have_BASE)
    tispeech_data(_base base_data.c extract_base.py
                "${TISPEECH_BASE_DLL}" "${CMAKE_CURRENT_BINARY_DIR}/base_data.c"
                --symbol sv_base_tables)
    add_library(tispeech_base_data STATIC "${_base}")
    target_link_libraries(tispeech_base_data PUBLIC tispeech_frames)

    add_executable(test_frames tests/test_frames.c)
    target_link_libraries(test_frames PRIVATE tispeech_base_data)
    add_test(NAME frames COMMAND test_frames)
else()
    message(STATUS
        "TISPEECH_BASE_DLL not set — building without base coefficient data. "
        "The frame tests need it; re-run with "
        "-DTISPEECH_BASE_DLL=/path/to/TIBASE32.DLL")
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_dsp.c")
    add_executable(test_dsp tests/test_dsp.c)
    target_link_libraries(test_dsp PRIVATE tispeech_dsp)
    add_test(NAME dsp COMMAND test_dsp)
endif()

if(_have_ENG)
    tispeech_data(_eng_img eng_image.c extract_images.py
                "${TISPEECH_ENG_DLL}" "${CMAKE_CURRENT_BINARY_DIR}/eng_image.c" --symbol sv_eng_image
                --language --text-range 1c205fb9:1c206780
                --text-range 1c209c20:1c249310)
    add_library(tispeech_frontend_eng STATIC "${_eng_img}")
    target_link_libraries(tispeech_frontend_eng PUBLIC tispeech_narrate)

    tispeech_data(_gen eng_lang_data.c extract_lang.py
                "${TISPEECH_ENG_DLL}" "${CMAKE_CURRENT_BINARY_DIR}/eng_lang_data.c"
                --symbol sv_lang_data_eng)
    add_library(tispeech_lang_eng STATIC "${_gen}")
    target_link_libraries(tispeech_lang_eng PUBLIC tispeech_rules)

    # Generator tables: the 9x16 interpolation-rate table and the two 110-entry
    # phoneme definition tables. Same data-source selection as the
    # rule tables above.
    tispeech_data(_gen_eng eng_gen_data.c extract_generator.py
                "${TISPEECH_ENG_DLL}" "${CMAKE_CURRENT_BINARY_DIR}/eng_gen_data.c"
                --language eng --symbol sv_gen_data_eng)
    add_library(tispeech_gen_eng STATIC "${_gen_eng}")
    target_link_libraries(tispeech_gen_eng PUBLIC tispeech_generator)

    add_executable(svphon tools/svphon.c)
    target_link_libraries(svphon PRIVATE tispeech_lang_eng)

    enable_testing()
    add_executable(test_ruleset tests/test_ruleset.c)
    target_link_libraries(test_ruleset PRIVATE tispeech_lang_eng)
    add_test(NAME ruleset COMMAND test_ruleset)
else()
    message(STATUS
        "TISPEECH_ENG_DLL not set — building without English rule data. "
        "The English CLI and rule tests need language data; re-run with "
        "-DTISPEECH_ENG_DLL=/path/to/TIENG32.DLL")
endif()

if(TARGET tispeech_gen_eng)
    target_link_libraries(test_generator PRIVATE tispeech_gen_eng)
    target_compile_definitions(test_generator PRIVATE TISPEECH_HAVE_ENG_GEN_DATA=1)
else()
    target_link_libraries(test_generator PRIVATE tispeech_generator)
endif()

# ---------------------------------------------------------------------------
# English synthesis data: TIBASE32's and TIENG32's .data sections whole, plus
# the tables TIENG32 embeds in .text after its frame generator. Needs both
# language datasets. Bundled by default; optionally extracted at build time.
# ---------------------------------------------------------------------------
if(_have_BASE AND _have_ENG)
    tispeech_data(_base_img base_image.c extract_images.py
                "${TISPEECH_BASE_DLL}" "${CMAKE_CURRENT_BINARY_DIR}/base_image.c" --symbol sv_base_image)
    add_library(tispeech_synth_eng STATIC "${_base_img}")
    target_link_libraries(tispeech_synth_eng PUBLIC tispeech_narrate tispeech_base_data)

endif()

if(_have_SPAN)
    tispeech_data(_span span_lang_data.c extract_lang.py
                "${TISPEECH_SPAN_DLL}" "${CMAKE_CURRENT_BINARY_DIR}/span_lang_data.c"
                --language span --symbol sv_lang_data_span)
    add_library(tispeech_lang_span STATIC "${_span}")
    target_link_libraries(tispeech_lang_span PUBLIC tispeech_rules)

    # Spanish's own real .data image (LoadLanguage descriptor and the
    # phoneme/voice/rewrite-rule tables sv_langmod_init resolves) -- same
    # extract_images.py mechanism as English's _eng_img, no per-language
    # code needed in that tool. Independent of the merged image below.
    tispeech_data(_span_desc_img span_desc_image.c extract_images.py
                "${TISPEECH_SPAN_DLL}" "${CMAKE_CURRENT_BINARY_DIR}/span_desc_image.c" --symbol sv_span_desc_image
                --language)
    add_library(tispeech_span_desc STATIC "${_span_desc_img}")
    target_link_libraries(tispeech_span_desc PUBLIC tispeech_narrate)
endif()

# ---------------------------------------------------------------------------
# Spanish text front end (src/textphon_span.c) and frame-generator relocation
# (src/langgen.c, shared with English — see REVERSING.md "Spanish
# synthesis"). Uses BOTH datasets: TISPAN32 for its own real tables,
# TIENG32 because langgen.c's ~200 module globals are addressed by ITS
# literal virtual addresses (tools/extract_span_frontend.py's header explains
# why one merged image serves both files). Linked into tispeech_shared below.
# ---------------------------------------------------------------------------
if(_have_ENG AND _have_SPAN)
    tispeech_data(_span_img span_image.c extract_span_frontend.py
                "${TISPEECH_ENG_DLL}" "${TISPEECH_SPAN_DLL}" "${CMAKE_CURRENT_BINARY_DIR}/span_image.c"
                --symbol sv_span_image)
    add_library(tispeech_frontend_span STATIC "${_span_img}")
    target_link_libraries(tispeech_frontend_span PUBLIC tispeech_narrate tispeech_lang_span)

    add_executable(svphon_span tools/svphon_span.c)
    target_link_libraries(svphon_span PRIVATE tispeech_frontend_span tispeech_span_desc)
endif()

# ---------------------------------------------------------------------------
# Shared library for managed (P/Invoke) callers.
#
# Exposes include/tispeech/capi.h — OUR ABI, not SoftVoice's. It always builds,
# with or without language data: tispeech_capabilities() reports what the build
# can actually do, so the .NET side gates on real capability instead of on the
# host operating system. Symbols are hidden unless marked TISPEECH_API.
# ---------------------------------------------------------------------------
add_library(tispeech_shared SHARED src/capi.c)
set_target_properties(tispeech_shared PROPERTIES
    OUTPUT_NAME tispeech
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
# Avoid colliding with the managed TiSpeech.dll on case-insensitive systems.
if(WIN32)
    set_target_properties(tispeech_shared PROPERTIES PREFIX "lib")
endif()
target_include_directories(tispeech_shared PUBLIC include)
# The whole engine is always built; the tables are compiled in below when the
# extracted data is enabled (the default), and otherwise can come from
# tispeech_load_dlls() at run time.
target_link_libraries(tispeech_shared PRIVATE tispeech_narrate)

add_executable(svsay tools/svsay.c)
target_link_libraries(svsay PRIVATE tispeech_shared)
# Find libtispeech next to svsay, in the build tree and wherever both are copied.
if(APPLE)
    set_target_properties(svsay PROPERTIES INSTALL_RPATH "@loader_path")
else()
    set_target_properties(svsay PROPERTIES INSTALL_RPATH "$ORIGIN")
endif()
set_target_properties(svsay PROPERTIES BUILD_WITH_INSTALL_RPATH ON)

if(_have_ENG)
    target_link_libraries(tispeech_shared PRIVATE tispeech_lang_eng tispeech_frontend_eng)
    target_compile_definitions(tispeech_shared PRIVATE TISPEECH_HAVE_ENG=1)
endif()

if(_have_SPAN)
    if(NOT TARGET tispeech_frontend_span)   # which links it itself
        target_link_libraries(tispeech_shared PRIVATE tispeech_lang_span)
    endif()
    target_compile_definitions(tispeech_shared PRIVATE TISPEECH_HAVE_SPAN=1)
endif()

if(TARGET tispeech_synth_eng)
    target_link_libraries(tispeech_shared PRIVATE tispeech_synth_eng)
    target_compile_definitions(tispeech_shared PRIVATE TISPEECH_HAVE_SYNTH_ENG=1)
endif()

# The reconstructed Spanish front end replaces the matcher-only path, and
# with TIBASE32 also gives Spanish synthesis (src/langgen.c's Spanish rules,
# src/duration_span.c).
if(TARGET tispeech_frontend_span)
    target_link_libraries(tispeech_shared PRIVATE tispeech_frontend_span tispeech_span_desc)
    target_compile_definitions(tispeech_shared PRIVATE TISPEECH_HAVE_FRONTEND_SPAN=1)
    if(TARGET tispeech_synth_eng)
        target_compile_definitions(tispeech_shared PRIVATE TISPEECH_HAVE_SYNTH_SPAN=1)
    endif()
endif()

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_capi.c")
    enable_testing()
    add_executable(test_capi tests/test_capi.c)
    target_link_libraries(test_capi PRIVATE tispeech_shared)
    target_include_directories(test_capi PRIVATE include)
    if(_have_ENG)
        target_link_libraries(test_capi PRIVATE tispeech_lang_eng)
        target_compile_definitions(test_capi PRIVATE TISPEECH_HAVE_ENG=1)
    endif()
    if(_have_SPAN)
        target_link_libraries(test_capi PRIVATE tispeech_lang_span)
        target_compile_definitions(test_capi PRIVATE TISPEECH_HAVE_SPAN=1)
    endif()
    if(TARGET tispeech_synth_eng)
        target_compile_definitions(test_capi PRIVATE TISPEECH_HAVE_SYNTH_ENG=1)
    endif()
    if(TARGET tispeech_frontend_span)
        target_compile_definitions(test_capi PRIVATE TISPEECH_HAVE_FRONTEND_SPAN=1)
        if(TARGET tispeech_synth_eng)
            target_compile_definitions(test_capi PRIVATE TISPEECH_HAVE_SYNTH_SPAN=1)
        endif()
    endif()
    add_test(NAME capi COMMAND test_capi)
    # The same suite on tables read at run time from the same DLLs, after
    # checking they synthesise exactly what the compiled-in ones do.
    if(TARGET tispeech_synth_eng AND TARGET tispeech_frontend_span
       AND TISPEECH_BASE_DLL AND TISPEECH_ENG_DLL AND TISPEECH_SPAN_DLL)
        add_test(NAME capi_loaded COMMAND test_capi
            "${TISPEECH_BASE_DLL}" "${TISPEECH_ENG_DLL}" "${TISPEECH_SPAN_DLL}")
    endif()
endif()

# The run-time loader against the build-time extractors, byte for byte.
if(TARGET tispeech_synth_eng AND TARGET tispeech_frontend_span
       AND TISPEECH_BASE_DLL AND TISPEECH_ENG_DLL AND TISPEECH_SPAN_DLL)
    add_executable(test_dllload tests/test_dllload.c)
    target_link_libraries(test_dllload PRIVATE tispeech_synth_eng tispeech_frontend_eng
        tispeech_frontend_span tispeech_span_desc tispeech_lang_eng tispeech_lang_span
        tispeech_base_data)
    add_test(NAME dllload COMMAND test_dllload
        "${TISPEECH_BASE_DLL}" "${TISPEECH_ENG_DLL}" "${TISPEECH_SPAN_DLL}")
endif()

# Development-only oracles. These separate shared libraries expose internal C
# functions to ctypes; the application never links an emulator or these targets.
if(TISPEECH_VERIFY_ORIGINAL)
    if(NOT (TISPEECH_ENG_DLL OR TISPEECH_SPAN_DLL OR TISPEECH_BASE_DLL))
        message(FATAL_ERROR "TISPEECH_VERIFY_ORIGINAL requires at least one original DLL")
    endif()
    if(NOT TISPEECH_VERIFY_CASES MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "TISPEECH_VERIFY_CASES must be a positive integer")
    endif()
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import unicorn, pefile"
        RESULT_VARIABLE _oracle_imports OUTPUT_QUIET ERROR_VARIABLE _oracle_error)
    if(NOT _oracle_imports EQUAL 0)
        message(FATAL_ERROR
            "Differential tests need unicorn and pefile in Python3_EXECUTABLE: ${_oracle_error}")
    endif()

    function(tispeech_oracle_library target)
        add_library(${target} SHARED ${ARGN})
        target_include_directories(${target} PRIVATE include)
        set_target_properties(${target} PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
    endfunction()

    function(tispeech_oracle_test name script target)
        add_test(NAME ${name} COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/${script}"
            --library "$<TARGET_FILE:${target}>" ${ARGN})
        set_tests_properties(${name} PROPERTIES LABELS differential TIMEOUT 600)
    endfunction()

    if(TISPEECH_ENG_DLL)
        tispeech_oracle_library(verify_rules_eng src/ruleset.c "${_gen}")
        add_dependencies(verify_rules_eng tispeech_lang_eng)
        tispeech_oracle_test(original_rules_eng verify_ruleset.py verify_rules_eng
            --dll "${TISPEECH_ENG_DLL}" --language eng
            --words= --cases 0 --random-cases "${TISPEECH_VERIFY_CASES}")

        tispeech_oracle_library(verify_generator src/generator.c)
        tispeech_oracle_test(original_generator verify_generator.py verify_generator
            --dll "${TISPEECH_ENG_DLL}" --cases "${TISPEECH_VERIFY_CASES}")
    endif()
    if(TISPEECH_SPAN_DLL)
        tispeech_oracle_library(verify_rules_span src/ruleset.c "${_span}")
        add_dependencies(verify_rules_span tispeech_lang_span)
        tispeech_oracle_test(original_rules_span verify_ruleset.py verify_rules_span
            --dll "${TISPEECH_SPAN_DLL}" --language span
            --words= --cases 0 --random-cases "${TISPEECH_VERIFY_CASES}")
    endif()
    if(TISPEECH_BASE_DLL)
        tispeech_oracle_library(verify_frames src/dsp.c src/frames.c)
        tispeech_oracle_test(original_frames verify_frames.py verify_frames
            --dll "${TISPEECH_BASE_DLL}" --cases "${TISPEECH_VERIFY_CASES}" --check-extract)
        tispeech_oracle_test(original_dsp verify_dsp.py verify_frames
            --dll "${TISPEECH_BASE_DLL}" --cases "${TISPEECH_VERIFY_CASES}")

        tispeech_oracle_library(verify_smoothing src/smoothing.c)
        tispeech_oracle_test(original_smoothing verify_smoothing.py verify_smoothing
            --dll "${TISPEECH_BASE_DLL}" --cases "${TISPEECH_VERIFY_CASES}")
    endif()
    if(TARGET tispeech_synth_eng AND TISPEECH_BASE_DLL AND TISPEECH_ENG_DLL)
        # The whole narrate pipeline against the original engine run end to
        # end under Unicorn: every stage boundary, then the PCM stream.
        tispeech_oracle_library(verify_narrate tools/narrate_oracle.c
            src/narrate.c src/duration_eng.c src/langgen.c src/engine.c
            src/expression.c src/smoothing.c src/frames.c src/dsp.c
            "${_base_img}" "${_eng_img}" "${_base}")
        get_filename_component(_dll_dir "${TISPEECH_BASE_DLL}" DIRECTORY)
        tispeech_oracle_test(original_voice verify_voice.py tispeech_shared
            --dlls "${_dll_dir}")
        tispeech_oracle_test(original_narrate verify_narrate.py verify_narrate
            --dlls "${_dll_dir}" --pcm --words 40
            --frontend-library "$<TARGET_FILE:tispeech_shared>")
        # Renderer events (word marks, syncs, mouth shapes) against the
        # original's own queue calls, 0x1c00498f.
        tispeech_oracle_test(original_events verify_narrate.py verify_narrate
            --dlls "${_dll_dir}" --events --words 10
            --frontend-library "$<TARGET_FILE:tispeech_shared>")

        # User dictionary (src/userdict.c): its own small oracle, since the
        # public tispeech_text_to_phonemes() (capi.c) doesn't take one.
        # sv_emu.py's Emu always loads TIBASE32.DLL, so this needs the same
        # DLL directory as the pipeline tests above, not TISPEECH_ENG_DLL alone.
        tispeech_oracle_library(verify_userdict tools/userdict_oracle.c
            src/textphon_eng.c src/userdict.c src/ruleset.c src/langgen.c
            src/engine.c "${_eng_img}")
        tispeech_oracle_test(original_userdict verify_narrate.py verify_userdict
            --dlls "${_dll_dir}" --library dummy
            --user-dict-library "$<TARGET_FILE:verify_userdict>")
    endif()

    # Spanish text front end (src/textphon_span.c), against TISPAN32 selected
    # through the original engine's own SVOpenSpeech(lang=2) -- see
    # tools/sv_emu.py's Emu(languages=...) / .open(lang=...). Needs the
    # merged generator/front-end image (tools/extract_span_frontend.py) and
    # TISPAN32's own real .data image (sv_langmod_init's descriptor), so
    # gated on tispeech_frontend_span itself (TISPEECH_ENG_DLL AND
    # TISPEECH_SPAN_DLL) rather than TISPEECH_SPAN_DLL alone.
    if(TARGET tispeech_frontend_span AND TISPEECH_BASE_DLL AND TISPEECH_ENG_DLL AND TISPEECH_SPAN_DLL)
        tispeech_oracle_library(verify_frontend_span tools/textphon_span_oracle.c
            src/textphon_span.c src/userdict.c src/ruleset.c src/langgen.c
            src/engine.c "${_span_img}" "${_span_desc_img}" "${_span}")
        add_dependencies(verify_frontend_span tispeech_frontend_span tispeech_span_desc)
        tispeech_oracle_test(original_frontend_span verify_spanish.py verify_frontend_span
            --dlls "${_dll_dir}" --words 100 --numbers 150)
        tispeech_oracle_library(verify_narrate_span tools/narrate_oracle.c
            src/narrate.c src/duration_span.c src/langgen.c src/engine.c
            src/expression.c src/smoothing.c src/frames.c src/dsp.c
            "${_base_img}" "${_span_img}" "${_span_desc_img}" "${_base}")
        target_compile_definitions(verify_narrate_span PRIVATE TISPEECH_ORACLE_SPAN=1)
        add_dependencies(verify_narrate_span tispeech_frontend_span tispeech_span_desc tispeech_synth_eng)
        tispeech_oracle_test(original_narrate_span verify_spanish.py verify_frontend_span
            --dlls "${_dll_dir}" --narrate-library "$<TARGET_FILE:verify_narrate_span>"
            --public-library "$<TARGET_FILE:tispeech_shared>"
            --words 40 --random 100 --numbers 40)
        # Every personality through {voice ...}: all four voice types, the
        # breathy source and both alternate F0 modes, past 256 frames.
        tispeech_oracle_test(original_narrate_span_voices verify_spanish.py verify_frontend_span
            --dlls "${_dll_dir}" --narrate-library "$<TARGET_FILE:verify_narrate_span>"
            --voices)
        tispeech_oracle_test(original_events_span verify_spanish.py verify_frontend_span
            --dlls "${_dll_dir}" --narrate-library "$<TARGET_FILE:verify_narrate_span>"
            --public-library "$<TARGET_FILE:tispeech_shared>"
            --events --numbers 10 --random 10)
    endif()
endif()
