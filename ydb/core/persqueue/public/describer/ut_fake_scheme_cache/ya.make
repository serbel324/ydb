UNITTEST_FOR(ydb/core/persqueue/public/describer)

FORK_SUBTESTS()
SPLIT_FACTOR(2)
SIZE(MEDIUM)

YQL_LAST_ABI_VERSION()

SRCS(
    ../describer_fake_scheme_cache_ut.cpp
)

PEERDIR(
    library/cpp/testing/unittest
    ydb/core/base
    ydb/core/testlib/basics/appdata
    ydb/core/testlib/basics/runtime
    ydb/core/tx/scheme_cache
    ydb/library/aclib
    yql/essentials/sql/pg_dummy
)

END()
