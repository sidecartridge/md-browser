# miniz: single-file public-domain DEFLATE/ZIP library (vendored, 3.0.2).
# We use only the ZIP *reader* (extraction), backed by a custom read callback
# so archives are read from FatFs rather than stdio.
add_library(miniz
    miniz.c
)

target_include_directories(miniz PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Trim miniz to reader-only, no host dependencies:
#  - NO_STDIO: drop the FILE*-based helpers; we supply a FatFs read callback
#  - NO_TIME: no time.h (the device has no wall clock)
#  - NO_ARCHIVE_WRITING_APIS: extraction only, no compression/writing
target_compile_definitions(miniz PUBLIC
    MINIZ_NO_STDIO
    MINIZ_NO_TIME
    MINIZ_NO_ARCHIVE_WRITING_APIS
)

target_link_libraries(miniz
    pico_stdlib
)
