# Turns a file into a C byte array, so web assets live as real files in the repo (no C escaping):
#   cmake -DIN=<file> -DOUT=<c file> -DNAME=<symbol> -P src/web/embed.cmake
# defines `const unsigned char NAME[]` and `const size_t NAME_len`: the file gzipped (http.c serves it with
# Content-Encoding: gzip, which every browser takes), about a quarter of its size in flash and on the air.
# The gzip is written beside the C file as NAME.gz, which the C file takes with #embed (C23; GCC 15 takes it
# in C11 too): the compiler reads its bytes, nothing turns them into text.
cmake_minimum_required(VERSION 3.19) # (file(ARCHIVE_CREATE ... FORMAT raw COMPRESSION GZip COMPRESSION_LEVEL))
get_filename_component(dir "${OUT}" DIRECTORY)
file(ARCHIVE_CREATE OUTPUT "${dir}/${NAME}.gz" PATHS "${IN}" FORMAT raw COMPRESSION GZip COMPRESSION_LEVEL 9)
# The gzip header's first 4 bytes as they are, then its time (bytes 4-7) as 0 ("none", RFC 1952) instead of
# the file's: the same page gives the same image, and the CI's ccache finds these files again.
file(READ "${dir}/${NAME}.gz" head LIMIT 4 HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " head "${head}")
# The page's SHA-256 in the C file: a new page is a new C file, also to a ccache that doesn't follow #embed.
file(SHA256 "${IN}" sum)
file(WRITE "${OUT}"
    "// Generated from ${IN} (SHA-256 ${sum}) by src/web/embed.cmake (gzipped): edit that file instead.\n"
    "#include <stddef.h>\n"
    "const unsigned char ${NAME}[] = {\n"
    "    ${head}0x00, 0x00, 0x00, 0x00,\n"
    "#embed \"${NAME}.gz\" gnu::offset(8)\n"
    "};\n"
    "const size_t ${NAME}_len = sizeof(${NAME});\n")
