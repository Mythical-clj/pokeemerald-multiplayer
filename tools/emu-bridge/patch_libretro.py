#!/usr/bin/env python3
"""Hooks owl_bridge into mGBA's libretro core. Usage: patch_libretro.py <mgba checkout>"""
import shutil, sys, os

root = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
dst = os.path.join(root, "src/platform/libretro")
for f in ("owl_bridge.c", "owl_bridge.h"):
    shutil.copy(os.path.join(here, f), dst)

p = os.path.join(dst, "libretro.c")
s = open(p).read()

def sub(a, b):
    global s
    assert s.count(a) == 1, a
    s = s.replace(a, b)

sub("static struct mCore* core;", '#include "owl_bridge.h"\nstatic struct mCore* core;')
sub("\tcore->reset(core);\n\t_setupMaps(core);\n\n\treturn true;",
    "\tcore->reset(core);\n\t_setupMaps(core);\n"
    "\t{\n\t\tconst char* owlDir = NULL;\n"
    "\t\tif (!environCallback(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &owlDir) || !owlDir) {\n\t\t\towlDir = \".\";\n\t\t}\n"
    "\t\towl_bridge_attach(core, owlDir);\n\t}\n\n\treturn true;")
sub("\tcore->runFrame(core);\n\tunsigned width, height;",
    "\towl_bridge_frame(core);\n\tcore->runFrame(core);\n\tunsigned width, height;")
sub("\tmCoreConfigDeinit(&core->config);\n\tcore->deinit(core);\n\tmappedMemoryFree(data, dataSize);",
    "\towl_bridge_detach();\n\tmCoreConfigDeinit(&core->config);\n\tcore->deinit(core);\n\tmappedMemoryFree(data, dataSize);")
open(p, "w").write(s)

# Link mbedTLS (for wss://) when the build is given OWL_MBEDTLS_DIR.
cm = os.path.join(root, "CMakeLists.txt")
c = open(cm).read()
anchor = "\ttarget_link_libraries(${BINARY_NAME}_libretro ${OS_LIB})\n"
assert c.count(anchor) == 1
c = c.replace(anchor, anchor + """\tif(OWL_MBEDTLS_DIR)
\t\tset(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)\n\t\tset(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
\t\tset(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
\t\tset(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
\t\tset(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
\t\tadd_subdirectory(${OWL_MBEDTLS_DIR} ${CMAKE_BINARY_DIR}/mbedtls EXCLUDE_FROM_ALL)
\t\ttarget_compile_definitions(${BINARY_NAME}_libretro PRIVATE OWL_USE_TLS)
\t\ttarget_link_libraries(${BINARY_NAME}_libretro mbedtls mbedx509 mbedcrypto)
\tendif()
""")
open(cm, "w").write(c)
print("patched", p, "and CMakeLists.txt")
