# Cruller's version and build switches, the same for every board (its CMakeLists.txt includes this).
#
# The version for local builds (override with -DCRULLER_VERSION=X.Y.Z). CI takes it from GitVersion
# (GitVersion.yml) instead. It must grow with every release: the Pico 2 W's boot ROM picks its A/B slot
# by it.
set(CRULLER_VERSION_DEFAULT "0.0.4")
set(CRULLER_VERSION "${CRULLER_VERSION_DEFAULT}" CACHE STRING "Firmware version (X.Y.Z or X.Y.Z-label.N)")
# The developer tools (src/platform/platform.h; 16 KB of the Pico 2 W's RAM): the CI builds them into
# pre-releases and pull requests, not into releases.
option(CRULLER_DEBUG "The developer tools (src/platform/platform.h)" ON)

# Both for <target>'s sources (<INTERFACE|PRIVATE>), called where it's defined (source properties are per
# directory): the version for version.c alone, so a new one compiles that file again and no other (the CI's
# ccache keeps the rest); CRULLER_DEBUG=0 when the developer tools are left out.
function(cruller_defs TARGET SCOPE)
    set_source_files_properties(${CRULLER_ROOT}/src/core/version.c PROPERTIES COMPILE_DEFINITIONS CRULLER_VERSION="${CRULLER_VERSION}")
    if (NOT CRULLER_DEBUG)
        target_compile_definitions(${TARGET} ${SCOPE} CRULLER_DEBUG=0)
    endif()
endfunction()
