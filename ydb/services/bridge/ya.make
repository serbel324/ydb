LIBRARY()

SRCS(
    grpc_service.cpp
)

PEERDIR(
    ydb/public/api/grpc
    ydb/library/grpc/server
    ydb/core/grpc_services
    ydb/core/grpc_services/base
    ydb/core/kesus/tablet
)

END()
