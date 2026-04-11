#pragma once

#include <google/protobuf/message.h>
#include <util/generic/string.h>

namespace NSQLFormat {

// Returns a copy of the SQL parse tree with string literals that may carry
// sensitive data replaced by '***removed***'. Numbers, identifiers, keywords
// and query shape are preserved so the result stays useful for logging and
// analytics. See sql_format_string_mask.cpp for the exact list of covered
// grammar rules and the rationale behind each.
TString MaskSqlStringLiterals(const NProtoBuf::Message& msg);

} // namespace NSQLFormat
