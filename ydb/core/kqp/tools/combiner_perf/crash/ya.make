PROGRAM(heap_use_after_free)

YQL_LAST_ABI_VERSION()


PEERDIR(
    library/cpp/dwarf_backtrace
    library/cpp/dwarf_backtrace/registry
)

SRCS(
    main.cpp
)

END()
