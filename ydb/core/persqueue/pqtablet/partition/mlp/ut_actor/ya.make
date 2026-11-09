UNITTEST_FOR(ydb/core/persqueue/pqtablet/partition/mlp)

YQL_LAST_ABI_VERSION()

FORK_SUBTESTS()
SPLIT_FACTOR(100)
SIZE(MEDIUM)

SRCS(
    ../ut/mlp_consumer_order_ut.cpp
    ../ut/mlp_consumer_wakeup_ut.cpp
    ../ut/mlp_storage_ut.cpp
)

PEERDIR(
    library/cpp/testing/unittest
    library/cpp/time_provider
    ydb/core/base
    ydb/core/keyvalue
    ydb/core/persqueue/events
    ydb/core/protos
    ydb/core/testlib/basics/appdata
    ydb/core/testlib/basics/runtime
    ydb/library/actors/core
    yql/essentials/sql/pg_dummy
)

END()
