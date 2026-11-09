LIBRARY()

SRCS(state_storage.cpp)

PEERDIR(
    ydb/core/base
    ydb/core/testlib/actors
    ydb/core/tx/scheme_board
)

IF (GCC)
    CFLAGS(
        -fno-devirtualize-speculatively
    )
ENDIF()

END()
