# The page's files, embedded gzipped in the firmware as C arrays (#embed, see embed.cmake): the same list
# for every board. cruller_web(<target> <INTERFACE|PRIVATE>) adds web_<file>.c (web_index_html.c...) to
# <target>, each generated into the build directory from its file here.
set(CRULLER_WEB_FILES sha256.js fw.js sd.js profiles.js mapper.js editor.js gameid.js rt4k_settings.json app.js index.html)

function(cruller_web TARGET SCOPE)
    set(web ${CMAKE_CURRENT_FUNCTION_LIST_DIR})
    foreach(file ${CRULLER_WEB_FILES})
        string(MAKE_C_IDENTIFIER "web_${file}" name) # (web_index_html, web_rt4k_settings_json...)
        set(out ${CMAKE_CURRENT_BINARY_DIR}/${name}.c)
        add_custom_command(OUTPUT ${out}
            BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/${name}.gz # (the C file's #embed)
            COMMAND ${CMAKE_COMMAND} -DIN=${web}/${file} -DOUT=${out} -DNAME=${name} -P ${web}/embed.cmake
            DEPENDS ${web}/${file} ${web}/embed.cmake
            VERBATIM)
        target_sources(${TARGET} ${SCOPE} ${out})
    endforeach()
endfunction()
