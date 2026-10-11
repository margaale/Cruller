# Turns a file into a C byte array, so web assets live as real files in the repo (no C escaping):
#   cmake -DIN=<file> -DOUT=<c file> -DNAME=<symbol> -P src/web/embed.cmake
# defines `const unsigned char NAME[]` and `const size_t NAME_len`: the file gzipped (http.c serves it with
# Content-Encoding: gzip, which every browser takes), about a quarter of its size in flash and on the air.
# The gzip is written beside the C file as NAME.gz, which the C file takes with #embed (C23; GCC 15 takes it
# in C11 too): the compiler reads its bytes, nothing turns them into text.
cmake_minimum_required(VERSION 3.19) # (file(ARCHIVE_CREATE ... FORMAT raw COMPRESSION GZip COMPRESSION_LEVEL))
get_filename_component(dir "${OUT}" DIRECTORY)
# Its header's time 0 ("none", RFC 1952) instead of now: the same page gives the same bytes, so the same
# image (and the CI's ccache finds these files again).
set(ENV{SOURCE_DATE_EPOCH} 0)
file(ARCHIVE_CREATE OUTPUT "${dir}/${NAME}.gz" PATHS "${IN}" FORMAT raw COMPRESSION GZip COMPRESSION_LEVEL 9)
file(WRITE "${OUT}"
    "// Generated from ${IN} by src/web/embed.cmake (gzipped): edit that file instead.\n"
    "#include <stddef.h>\n"
    "const unsigned char ${NAME}[] = {\n"
    "#embed \"${NAME}.gz\"\n"
    "};\n"
    "const size_t ${NAME}_len = sizeof(${NAME});\n")
