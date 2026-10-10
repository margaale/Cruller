# Turns a file into a C byte array, so web assets live as real files in the repo (no C escaping):
#   cmake -DIN=<file> -DOUT=<c file> -DNAME=<symbol> -P src/web/embed.cmake
# defines `const unsigned char NAME[]` and `const size_t NAME_len`: the file gzipped (http.c serves it with
# Content-Encoding: gzip, which every browser takes), about a quarter of its size in flash and on the air.
cmake_minimum_required(VERSION 3.19) # (file(ARCHIVE_CREATE ... FORMAT raw COMPRESSION GZip COMPRESSION_LEVEL))
file(ARCHIVE_CREATE OUTPUT "${OUT}.gz" PATHS "${IN}" FORMAT raw COMPRESSION GZip COMPRESSION_LEVEL 9)
file(READ "${OUT}.gz" hex HEX)
file(REMOVE "${OUT}.gz")
# Without the gzip header's time (bytes 4-7; 0 is "none", RFC 1952): the same page gives the same image,
# and the CI's ccache finds these files again.
string(SUBSTRING "${hex}" 0 8 head)
string(SUBSTRING "${hex}" 16 -1 tail)
set(hex "${head}00000000${tail}")
string(LENGTH "${hex}" digits)
math(EXPR len "${digits} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUT}"
    "// Generated from ${IN} by src/web/embed.cmake (gzipped): edit that file instead.\n"
    "#include <stddef.h>\n"
    "const unsigned char ${NAME}[] = {${bytes}};\n"
    "const size_t ${NAME}_len = ${len};\n")
