UNITTEST_FOR(ydb/core/tx/scheme_board)

FORK_SUBTESTS()

SIZE(MEDIUM)

PEERDIR(
    library/cpp/testing/unittest
    yql/essentials/minikql/comp_nodes/llvm16
    ydb/core/testlib/basics/appdata
    ydb/core/testlib/basics/runtime
    ydb/core/testlib/basics/state_storage
    yql/essentials/sql/pg_dummy
)

YQL_LAST_ABI_VERSION()

SRCS(
    replica_ut.cpp
    ut_helpers.cpp
)

END()
