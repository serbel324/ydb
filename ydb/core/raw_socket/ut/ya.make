UNITTEST_FOR(ydb/core/raw_socket)

SIZE(small)
SRCS(
    buffered_writer_ut.cpp
    sock_listener_ut.cpp
)

PEERDIR(
    ydb/core/raw_socket
    ydb/core/testlib/basics/appdata
    ydb/core/testlib/basics/runtime
    yql/essentials/sql/pg_dummy
)

YQL_LAST_ABI_VERSION()
END()
