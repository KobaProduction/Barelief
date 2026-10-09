#include "core_app/scheme_editor/typed_parameter_helper.h"
#include <cassert>
#include <limits>
#include <bit>
using namespace signal_contracts;
int main() {
    NumericLiteral value=std::uint64_t{7};
    assert(numeric_editor::parse_literal("18446744073709551615",value,value));
    assert(std::get<std::uint64_t>(value)==std::numeric_limits<std::uint64_t>::max());
    assert(numeric_editor::literal_text(value)=="18446744073709551615");
    const auto old=value;
    for(const auto& invalid:{"18446744073709551616","-1","+1","01","1.0","1e3"," 1"}) {
        assert(!numeric_editor::parse_literal(invalid,value,value));assert(literal_equal(old,value));
    }
    value=std::int64_t{0};assert(numeric_editor::parse_literal("-9223372036854775808",value,value));
    assert(std::get<std::int64_t>(value)==std::numeric_limits<std::int64_t>::min());
    assert(numeric_editor::literal_text(value)=="-9223372036854775808");
    assert(!numeric_editor::parse_literal("-9223372036854775809",value,value));
    value=std::uint64_t{0};assert(numeric_editor::parse_literal("9007199254740993",value,value));
    assert(std::get<std::uint64_t>(value)==9007199254740993ULL);
    value=-0.0f;assert(numeric_editor::literal_text(value)=="-0");
    NumericLiteral out=0.0f;assert(numeric_editor::parse_literal(numeric_editor::literal_text(value),out,out));
    assert(literal_equal(value,out));
    value=std::bit_cast<float>(std::uint32_t{1});out=0.0f;
    assert(numeric_editor::parse_literal(numeric_editor::literal_text(value),out,out));assert(literal_equal(value,out));
    assert(!numeric_editor::parse_literal("nan",out,out));
}
