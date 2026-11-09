UNITTEST_FOR(ydb/core/kqp)

FORK_SUBTESTS()

SIZE(SMALL)

SRCS(
    kqp_tli_ut.cpp
    dynamic_function_registry_ut.cpp
)

PEERDIR(
    ydb/core/kqp/common
    yql/essentials/minikql
    yql/essentials/minikql/invoke_builtins/llvm16
    yql/essentials/public/udf/service/exception_policy
    yql/essentials/sql/pg_dummy
)

YQL_LAST_ABI_VERSION()

END()
