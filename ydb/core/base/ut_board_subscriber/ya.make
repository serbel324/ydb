UNITTEST_FOR(ydb/core/base)

FORK_SUBTESTS()

SIZE(MEDIUM)

PEERDIR(
    ydb/library/actors/interconnect
    ydb/library/actors/core
    library/cpp/testing/unittest
    ydb/core/base
    ydb/core/testlib/basics/appdata
    ydb/core/testlib/basics/runtime
    ydb/core/testlib/basics/state_storage
    yql/essentials/sql/pg_dummy
    yql/essentials/minikql/comp_nodes/llvm16
)

YQL_LAST_ABI_VERSION()

SRCS(
    board_subscriber_ut.cpp
)

END()
