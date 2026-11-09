UNITTEST_FOR(ydb/core/fq/libs/result_formatter)

FORK_SUBTESTS()

IF (SANITIZER_TYPE)
    SIZE(MEDIUM)
ENDIF()

SRCS(
    result_formatter_ut.cpp
)

PEERDIR(
    yql/essentials/sql/pg_dummy
    yql/essentials/public/udf/service/exception_policy
)

YQL_LAST_ABI_VERSION()

END()
