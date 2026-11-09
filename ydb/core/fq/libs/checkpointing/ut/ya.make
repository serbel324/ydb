UNITTEST_FOR(ydb/core/fq/libs/checkpointing)

SRCS(
    checkpoint_coordinator_ut.cpp
)

PEERDIR(
    library/cpp/testing/unittest
    ydb/core/fq/libs/checkpointing
    ydb/core/testlib/actors
    ydb/core/testlib/basics/appdata
    yql/essentials/sql/pg_dummy
)

SIZE(MEDIUM)

YQL_LAST_ABI_VERSION()

END()
