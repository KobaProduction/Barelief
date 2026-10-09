#pragma once
#include <signal_contracts/numeric_policy.h>
#include <QString>
namespace numeric_editor {
// Control-path text conversion. The destination is unchanged on refusal.
bool parse_literal(const QString&, const signal_contracts::NumericLiteral& type,
                   signal_contracts::NumericLiteral& out);
QString literal_text(const signal_contracts::NumericLiteral&);
}
