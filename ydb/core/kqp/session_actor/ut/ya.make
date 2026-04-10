UNITTEST()

FORK_SUBTESTS()

SIZE(SMALL)

SRCS(
    kqp_log_query_ut.cpp
)

PEERDIR(
    util/charset
    ydb/core/protos
)

END()
