#pragma once

#include <ydb/public/lib/ydb_cli/common/command.h>

#include <library/cpp/logger/priority.h>

namespace NYdb::NTPCC {

struct TRunConfig {
    TRunConfig(const NConsoleClient::TClientCommand::TConfig& connectionConfig);
    NConsoleClient::TClientCommand::TConfig ConnectionConfig;

    int WarehouseCount = 0;
    int WarmupSeconds = 0;
    int RunSeconds = 0;

    int MaxInflight = 0;

    TString Path;

    ELogPriority LogPriority = ELogPriority::TLOG_INFO;
    bool NoSleep = false;
};

void RunSync(const TRunConfig& config);

} // namespace NYdb::NTPCC
