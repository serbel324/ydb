UNITTEST_FOR(ydb/core/fq/libs/common)

FORK_SUBTESTS()

IF (SANITIZER_TYPE)
    SIZE(MEDIUM)
ENDIF()

SRCS(
    cache_ut.cpp
    entity_id_ut.cpp
    iceberg_processor_ut.cpp
    rows_proto_splitter_ut.cpp
    util_ut.cpp
)

PEERDIR(
    ydb/public/sdk/cpp/src/client/result
    ydb/public/sdk/cpp/src/client/value
    yql/essentials/sql/pg_dummy
)

YQL_LAST_ABI_VERSION()

END()
