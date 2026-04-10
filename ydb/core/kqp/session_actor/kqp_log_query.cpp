#include "kqp_log_query.h"

#include <ydb/core/kqp/common/events/query.h>
#include <ydb/core/kqp/session_actor/kqp_query_state.h>
#include <ydb/core/protos/kqp.pb.h>

#include <library/cpp/json/writer/json.h>
#include <util/charset/utf8.h>
#include <yql/essentials/public/issue/yql_issue_message.h>

namespace NKikimr::NKqp {
namespace {

constexpr size_t SQL_TEXT_MAX_SIZE = 4000;

#define _KQP_REQ_LOG(stream) LOG_TRACE_S(*TlsActivationContext, NKikimrServices::KQP_REQUEST, "[REQ_JSON] " << stream)

struct TJsonExtra {
    TStringBuf Database;
    TString QueryType;
    TString Action;
    TString ApplicationName;
    TStringBuf ClientAddress;
    TString CommandTag;
    ui64 ParametersSize = 0;
    // Completed-only fields
    TString Status;
    i64 DurationMs = -1;
    i64 CpuTimeUs = -1;
    i8 CompileCacheHit = -1; // -1 unknown, 0 miss, 1 hit
};

void WriteJsonChunks(TStringBuf poolId, const TString& reqId, TStringBuf sessionId, TStringBuf userSID,
                     TStringBuf eventName, TStringBuf requestText,
                     const NYql::TIssues& issues,
                     const TJsonExtra& extra)
{
    // Split requestText into UTF-8-safe chunks using standard Utf8TruncateRobust
    std::vector<TStringBuf> chunks;
    if (requestText.empty()) {
        chunks.emplace_back();
    } else {
        TStringBuf remaining = requestText;
        while (!remaining.empty()) {
            TStringBuf chunk = Utf8TruncateRobust(remaining, SQL_TEXT_MAX_SIZE);
            if (chunk.empty()) {
                // Degenerate case: first byte is invalid UTF-8, skip it to make progress
                chunk = remaining.Head(std::min(remaining.size(), SQL_TEXT_MAX_SIZE));
            }
            chunks.push_back(chunk);
            remaining.Skip(chunk.size());
        }
    }

    const size_t total = chunks.size();

    for (size_t i = 0; i < total; ++i) {
        TStringStream ss;
        NJsonWriter::TBuf json(NJsonWriter::HEM_RELAXED, &ss);

        json.BeginObject();
        json.WriteKey("req_id").WriteString(reqId);
        json.WriteKey("pool").WriteString(poolId);
        json.WriteKey("session").WriteString(sessionId);
        json.WriteKey("user").WriteString(userSID);
        json.WriteKey("part").WriteInt(i + 1);
        json.WriteKey("total").WriteInt(total);

        json.WriteKey("request").BeginObject();
        json.WriteKey("event").WriteString(eventName);

        if (!chunks[i].empty()) {
            json.WriteKey("data").WriteString(chunks[i]);
        }

        if (!issues.Empty()) {
            json.WriteKey("issues").WriteString(issues.ToOneLineString());
        }

        // Write extra metadata only in the first chunk
        if (i == 0) {
            if (extra.Database) {
                json.WriteKey("database").WriteString(extra.Database);
            }
            if (extra.QueryType) {
                json.WriteKey("query_type").WriteString(extra.QueryType);
            }
            if (extra.Action) {
                json.WriteKey("action").WriteString(extra.Action);
            }
            if (extra.ApplicationName) {
                json.WriteKey("application").WriteString(extra.ApplicationName);
            }
            if (extra.ClientAddress) {
                json.WriteKey("client_address").WriteString(extra.ClientAddress);
            }
            if (extra.CommandTag) {
                json.WriteKey("command_tag").WriteString(extra.CommandTag);
            }
            if (extra.ParametersSize > 0) {
                json.WriteKey("parameters_size").WriteULongLong(extra.ParametersSize);
            }
            if (extra.Status) {
                json.WriteKey("status").WriteString(extra.Status);
            }
            if (extra.DurationMs >= 0) {
                json.WriteKey("duration_ms").WriteLongLong(extra.DurationMs);
            }
            if (extra.CpuTimeUs >= 0) {
                json.WriteKey("cpu_time_us").WriteLongLong(extra.CpuTimeUs);
            }
            if (extra.CompileCacheHit >= 0) {
                json.WriteKey("compile_cache_hit").WriteBool(extra.CompileCacheHit == 1);
            }
        }

        json.EndObject();
        json.EndObject();

        _KQP_REQ_LOG(ss.Str());
    }
}

TString MakeRequestId(const TKqpQueryState& state) {
    auto res = TStringBuilder()
        << TActivationContext::Now().MicroSeconds() << "_";

    if (state.RequestEv) {
        res << (const void*)state.RequestEv->GetQuery().data();
    } else {
        res << (const void*)&state;
    }

    return res;
}

} // anonymous namespace

TString TLogQuery::LogStarted(const TKqpQueryState& state) {
    TString reqId = MakeRequestId(state);

    TLogQuery log([&state, reqId]() {
        TStringBuf poolId = state.UserRequestContext
            ? TStringBuf(state.UserRequestContext->PoolId)
            : TStringBuf{};

        TStringBuf sessionId = state.UserRequestContext
            ? TStringBuf(state.UserRequestContext->SessionId)
            : TStringBuf{};

        TString userSid = state.UserToken
            ? state.UserToken->GetUserSID()
            : TString{};

        auto query = state.ExtractQueryText();

        TJsonExtra extra;
        extra.Database = state.GetDatabase();
        extra.QueryType = NKikimrKqp::EQueryType_Name(state.GetType());
        extra.Action = NKikimrKqp::EQueryAction_Name(state.GetAction());
        extra.ApplicationName = state.ApplicationName.GetOrElse(TString{});
        extra.ClientAddress = state.ClientAddress;
        if (state.CommandTagName) {
            extra.CommandTag = *state.CommandTagName;
        }
        extra.ParametersSize = state.ParametersSize;

        WriteJsonChunks(
            poolId,
            reqId,
            sessionId,
            userSid,
            "started",
            query,
            {},
            extra
        );
    });
    log.Log();
    return reqId;
}

void TLogQuery::LogCompleted(const TKqpQueryState& state,
                              const NKikimrKqp::TEvQueryResponse& record,
                              const TString& reqId) {
    TLogQuery log([&state, &record, &reqId]() {
        TStringBuf sessionId = state.UserRequestContext
            ? TStringBuf(state.UserRequestContext->SessionId)
            : TStringBuf{};

        TString userSID = state.UserToken
            ? state.UserToken->GetUserSID()
            : TString{};

        auto queryText = state.ExtractQueryText();

        NYql::TIssues issues;
        TStringBuf poolId;

        if (record.HasResponse()) {
            poolId = TStringBuf(record.GetResponse().GetEffectivePoolId());
            NYql::IssuesFromMessage(record.GetResponse().GetQueryIssues(), issues);
        } else {
            poolId = state.UserRequestContext
                ? TStringBuf(state.UserRequestContext->PoolId)
                : TStringBuf{};
        }

        TJsonExtra extra;
        extra.Database = state.GetDatabase();
        extra.QueryType = NKikimrKqp::EQueryType_Name(state.GetType());
        extra.Action = NKikimrKqp::EQueryAction_Name(state.GetAction());
        extra.ApplicationName = state.ApplicationName.GetOrElse(TString{});
        extra.ClientAddress = state.ClientAddress;
        if (state.CommandTagName) {
            extra.CommandTag = *state.CommandTagName;
        }
        extra.ParametersSize = state.ParametersSize;
        extra.Status = Ydb::StatusIds::StatusCode_Name(record.GetYdbStatus());
        extra.DurationMs = (TActivationContext::Now() - state.StartTime).MilliSeconds();
        extra.CpuTimeUs = state.CpuTime.MicroSeconds();
        extra.CompileCacheHit = state.CompileStats.FromCache ? 1 : 0;

        WriteJsonChunks(
            poolId,
            reqId,
            sessionId,
            userSID,
            "completed",
            queryText,
            issues,
            extra
        );
    });
    log.Log();
}

} // namespace NKikimr::NKqp
