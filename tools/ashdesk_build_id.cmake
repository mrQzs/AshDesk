# Run on every build by the ashdesk_build_id target (CMakeLists.txt): write the commit the binaries are built from
# into ashdesk_build.h, which `--version` prints. The file is rewritten only when its text changes, so an
# unchanged commit does not recompile anything. Outside a git work tree (sources copied to .3) the id is "unknown".
# Usage: cmake -DSRC=<repo> -DOUT=<header> -P ashdesk_build_id.cmake
execute_process(COMMAND git -C "${SRC}" rev-parse --short=10 HEAD
                OUTPUT_VARIABLE id OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE rc)
if(NOT rc EQUAL 0 OR id STREQUAL "")
  set(id "unknown")
else()
  # "-dirty" only for changes to what gets compiled; notes and docs edited locally do not count
  execute_process(COMMAND git -C "${SRC}" status --porcelain --untracked-files=no -- ui glass updater CMakeLists.txt
                  OUTPUT_VARIABLE changed OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT changed STREQUAL "")
    string(APPEND id "-dirty")
  endif()
endif()
set(text "/* Generated at build time by tools/ashdesk_build_id.cmake; do not edit. */\n#define ASHDESK_BUILD_ID \"${id}\"\n")
set(old "")
if(EXISTS "${OUT}")
  file(READ "${OUT}" old)
endif()
if(NOT old STREQUAL text)
  file(WRITE "${OUT}" "${text}")
endif()
