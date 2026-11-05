# Run with: cmake -DOUT=<file.cpp> -P BuildStamp.cmake
# Regenerates a source file holding the current time; run on every build so the
# GUI can show when the binary was actually built.
# string(TIMESTAMP) reports SOURCE_DATE_EPOCH instead of the current time when it
# is set, and flatpak-builder sets it to the modification time of the manifest.
unset(ENV{SOURCE_DATE_EPOCH})
string(TIMESTAMP BUILD_STAMP "%Y-%m-%d %H:%M:%S")
file(WRITE "${OUT}" "const char* bioslicer_build_timestamp() { return \"${BUILD_STAMP}\"; }\n")
