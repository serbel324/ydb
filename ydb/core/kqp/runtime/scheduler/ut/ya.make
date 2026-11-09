UNITTEST_FOR(ydb/core/kqp/runtime)

FORK_SUBTESTS()

SIZE(MEDIUM)
REQUIREMENTS(cpu:4)

SRCS(
    scheduler/kqp_compute_scheduler_ut.cpp
)

YQL_LAST_ABI_VERSION()

PEERDIR(
    library/cpp/testing/unittest
    ydb/core/kqp/counters
    yql/essentials/minikql/comp_nodes/llvm16
    yql/essentials/parser/pg_wrapper
    yql/essentials/public/udf/service/exception_policy
    yql/essentials/sql/pg
)

END()
