# Production (SCALE_PROTOCOL_CI2001) compiles ONLY the CAS CI-2001 parser. The
# other parsers exist for the QEMU suite and the legacy auto-detect option.
set(srcs "cas_ci2001_parser.c")
if(CONFIG_SCALE_PROTOCOL_AUTODETECT_LEGACY)
    list(APPEND srcs "asuki_parser.c" "flintec_parser.c" "asuki_k1_parser.c" "cas_parser.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    REQUIRES "interfaces"
)
