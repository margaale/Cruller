#pragma once

// The firmware's version ("0.6.1", "0.6.1-alpha.229"). Defined in version.c alone (built with
// CRULLER_VERSION), so a new version compiles that file again and no other (the CI's ccache keeps the rest).
extern const char cruller_version[];
