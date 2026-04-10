#pragma once

#include <ydb/core/protos/kqp_physical.pb.h>
#include <ydb/core/protos/kqp.pb.h>

#include <util/datetime/base.h>
#include <util/generic/string.h>
#include <util/stream/output.h>
#include <yql/essentials/public/issue/yql_issue.h>

#include <functional>

namespace NKikimrKqp {
class TEvQueryResponse;
}

namespace NKikimr::NKqp {

class TKqpQueryState;

// Replaces string literals in the SQL query with '***removed***' using
// the SQL parser (EFormatMode::ObfuscateWithStringMask). If parsing fails,
// returns the original query unchanged.
TString MaskSensitiveLiterals(const TString& query);

// Returns true if any transaction in the physical query contains a scheme operation
// that may carry sensitive data (CREATE/ALTER USER, CREATE/ALTER SECRET,
// CREATE/UPSERT/ALTER OBJECT with TYPE SECRET).
inline bool HasSensitiveSchemeOperation(const NKqpProto::TKqpPhyQuery& phyQuery) {
    for (const auto& tx : phyQuery.GetTransactions()) {
        if (!tx.HasSchemeOperation()) {
            continue;
        }
        const auto& op = tx.GetSchemeOperation();
        if (op.HasCreateUser() || op.HasAlterUser() ||
            op.HasCreateSecret() || op.HasAlterSecret()) {
            return true;
        }
        if (op.HasCreateObject() || op.HasUpsertObject() || op.HasAlterObject()) {
            if (op.GetObjectType() == "SECRET") {
                return true;
            }
        }
    }
    return false;
}

class TLogQuery {
public:
    using TAction = std::function<void()>;

    explicit TLogQuery(TAction action)
        : Action(std::move(action))
    {}

    void Log() const { if (Action) Action(); }

    // Logs "started" event, returns the generated req_id for correlation with Completed
    static TString LogStarted(const TKqpQueryState& state);

    static void LogCompleted(const TKqpQueryState& state,
                             const NKikimrKqp::TEvQueryResponse& record,
                             const TString& reqId);

    // For scripting queries forwarded to KqpWorkerActor: RequestEv is released
    // before the response arrives, so we log using saved fields instead.
    static void LogForwardedCompleted(const TString& queryText,
                                      const TString& database,
                                      NKikimrKqp::EQueryType queryType,
                                      NKikimrKqp::EQueryAction queryAction,
                                      TInstant startTime,
                                      const NKikimrKqp::TEvQueryResponse& record,
                                      const TString& reqId);

private:
    TAction Action;
};

#define KQP_REQ_LOG_ENABLED() \
    IS_CTX_LOG_PRIORITY_ENABLED(*TlsActivationContext, NActors::NLog::PRI_TRACE, NKikimrServices::KQP_REQUEST, 0ull)

} // namespace NKikimr::NKqp
