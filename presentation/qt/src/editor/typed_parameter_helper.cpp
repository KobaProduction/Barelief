#include "core_app/scheme_editor/typed_parameter_helper.h"
#include <charconv>
#include <cmath>
#include <limits>
#include <array>
#include <type_traits>
namespace numeric_editor {
namespace {
template<class T>
bool valid_integer_spelling(const char* begin,const char* end) {
    if constexpr(!std::is_integral_v<T>)return true;
    const char* digits=begin;
    if constexpr(std::is_signed_v<T>)if(*digits=='-')++digits;
    if(digits==end || (*digits=='0' && digits+1!=end) || (digits!=begin && *digits=='0'))return false;
    for(auto* p=digits;p!=end;++p)if(*p<'0' || *p>'9')return false;
    return true;
}

template<class T>
bool parse_typed_literal(const QByteArray& bytes,signal_contracts::NumericLiteral& candidate) {
    const char* begin=bytes.constData();
    const char* end=begin+bytes.size();
    if(!valid_integer_spelling<T>(begin,end))return false;
    T value{};
    const auto result=std::from_chars(begin,end,value);
    if(result.ec!=std::errc{} || result.ptr!=end)return false;
    if constexpr(std::is_floating_point_v<T>)if(!std::isfinite(value))return false;
    candidate=value;
    return true;
}
}
bool parse_literal(const QString& text,const signal_contracts::NumericLiteral& type,signal_contracts::NumericLiteral& out) {
    const auto bytes=text.toUtf8();
    if(bytes.isEmpty() || type.valueless_by_exception())return false;
    signal_contracts::NumericLiteral candidate;
    const bool valid=std::visit([&](const auto& sample) {
        using T=std::decay_t<decltype(sample)>;
        return parse_typed_literal<T>(bytes,candidate);
    },type);
    if(valid)out=candidate;
    return valid;
}
QString literal_text(const signal_contracts::NumericLiteral& value) {
    if(value.valueless_by_exception())return {};
    return std::visit([](const auto& number) {
        std::array<char,128> buffer{};
        auto result=std::to_chars(buffer.data(),buffer.data()+buffer.size(),number);
        return result.ec==std::errc{}?QString::fromLatin1(buffer.data(),int(result.ptr-buffer.data())):QString{};
    },value);
}
}
