# Cruller's version for local builds (override with -DCRULLER_VERSION=X.Y.Z). CI takes it from
# GitVersion (GitVersion.yml) instead. It must grow with every release: the Pico 2 W's boot ROM picks
# its A/B slot by it.
set(CRULLER_VERSION_DEFAULT "0.0.4")
