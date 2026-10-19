#include <project_storage/project_source.h>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <utility>

using namespace project_storage;
// Allocation refusal is exercised at each real production nothrow allocation.
// No fault-injection seam is added to the adapter or its public API.
static int fail_after = -1;
static void* test_allocate(std::size_t size) noexcept {
    if (fail_after == 0) return nullptr;
    if (fail_after > 0) --fail_after;
    return std::malloc(size ? size : 1);
}
void* operator new(std::size_t size) { auto* p=std::malloc(size ? size : 1); if(!p) std::abort(); return p; }
void* operator new[](std::size_t size) { return ::operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return test_allocate(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return test_allocate(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p,std::size_t) noexcept { std::free(p); }
void operator delete[](void* p,std::size_t) noexcept { std::free(p); }
void operator delete(void* p,const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p,const std::nothrow_t&) noexcept { std::free(p); }
static bool expect_condition(bool valid,int line,const char* expression) {
    if(!valid)std::printf("FAIL line %d: %s\n",line,expression);
    return valid;
}
#define CHECK(x) if(!::expect_condition(bool(x),__LINE__,#x)) return false
static NodeHandle member(const OwnedProjectSource& s, NodeHandle p, std::string_view k) {
    NodeHandle h; (void)find_unique_member(s, p, k, h); return h;
}
static NodeView node(const OwnedProjectSource& s, NodeHandle h) {
    NodeView v; (void)s.view(h, v); return v;
}
static bool ownership_and_lexemes() {
    const std::string expected = R"( {"ui":{"theme":"dark","huge":9007199254740993},"opaque":[18446744073709551615,-9223372036854775809,1e9999,-0,1.2300E+004]} )";
    std::string caller = expected;
    OwnedProjectSource source;
    CHECK(parse_project_source(caller, source));
    auto root = source.root();
    auto ui = member(source, root, "ui");
    auto huge = member(source, ui, "huge");
    caller.assign(100, 'x'); caller.clear(); caller.shrink_to_fit();
    OwnedProjectSource moved = std::move(source);
    CHECK(source.bytes().empty()); CHECK(moved.bytes() == expected);
    CHECK(node(moved, huge).raw == "9007199254740993");
    CHECK(node(moved, huge).offset == expected.find("9007199254740993"));
    auto a = node(moved, member(moved, root, "opaque"));
    std::array<std::string_view, 5> tokens = {"18446744073709551615", "-9223372036854775809", "1e9999", "-0", "1.2300E+004"};
    auto h = a.first_child;
    for (auto t : tokens) { auto v = node(moved,h); CHECK(v.raw == t); h = v.next_sibling; }
    CHECK(!h.source);
    OwnedProjectSource equal; CHECK(parse_project_source(expected,equal));
    NodeView unchanged; unchanged.offset = 42;
    CHECK(equal.view(huge,unchanged).code == Error::InvalidHandle); CHECK(unchanged.offset == 42);
    return true;
}
static bool grammar_and_unicode() {
    OwnedProjectSource source;
    CHECK(parse_project_source(R"({"\u0064type":"\uD83D\uDE00","nul":"a\u0000b","esc":"\"\\\/\b\f\n\r\t","raw":"Привет"})",source));
    auto dtype = member(source,source.root(),"dtype");
    CHECK(node(source,dtype).decoded == "\xf0\x9f\x98\x80");
    CHECK(node(source,member(source,source.root(),"nul")).decoded == std::string_view("a\0b",3));
    CHECK(node(source,member(source,source.root(),"esc")).decoded == std::string_view("\"\\/\b\f\n\r\t",8));
    CHECK(node(source,member(source,source.root(),"raw")).decoded == "Привет");
    const auto baseline = std::string(source.bytes()); const auto id = source.root();
    const std::array<std::string_view,27> invalid = {"", " ", "{", "[1,]", "{\"x\":1,}", "{\"x\" 1}", "true false", "01", "+1", ".1", "1.", "1e", "1e+", "--1", "NaN", "//x\n1", "/*x*/1", "[truefalse]", "\"\n\"", R"("\x01")", R"("\uD800")", R"("\uDC00")", R"("\uD800\u0041")", R"("\u000g")", "nullx", "[}", "{\"x\":}"};
    for (auto bad : invalid) { auto e = parse_project_source(bad,source); CHECK(!e); CHECK(e.offset <= bad.size()); CHECK(source.bytes() == baseline); CHECK(source.root() == id); }
    for (auto bad : {std::string("\"\xc0\xaf\"",4), std::string("\"\xed\xa0\x80\"",5), std::string("\"\xf4\x90\x80\x80\"",6), std::string("\"\x80\"",3), std::string("\"\xe2\x82\"",4)}) {
        CHECK(parse_project_source(bad,source).code == Error::InvalidUtf8); CHECK(source.bytes() == baseline);
    }
    for (auto good : {"null", "true", "false", "-0", "0.0", "-12.25e+99", "1E-9999", "[{},[],null,true,false]"}) CHECK(parse_project_source(good,source));
    return true;
}
static bool duplicates_and_integers() {
    OwnedProjectSource s;
    CHECK(parse_project_source(R"({"dtype":1,"\u0064type":2,"opaque":9007199254740993,"opaque":9007199254740992,"min":-9223372036854775808,"max":9223372036854775807,"u":18446744073709551615,"over":18446744073709551616,"negative":-1,"frac":1.0,"exp":1e0,"negzero":-0,"trap":9007199254740993})",s));
    const std::array<std::string_view,1> keys = {"dtype"};
    CHECK(validate_known_keys(s,s.root(),keys).code == Error::DuplicateKnownKey);
    NodeHandle out{99,99}; CHECK(find_unique_member(s,s.root(),"dtype",out).code == Error::AmbiguousKey); CHECK(out == (NodeHandle{99,99}));
    CHECK(find_unique_member(s,s.root(),"absent",out).code == Error::MissingMember);
    std::int64_t i = 17; std::uint64_t u = 17;
    CHECK(read_int64(s,member(s,s.root(),"min"),i)); CHECK(i == std::numeric_limits<std::int64_t>::min());
    CHECK(read_int64(s,member(s,s.root(),"max"),i)); CHECK(i == std::numeric_limits<std::int64_t>::max());
    CHECK(read_uint64(s,member(s,s.root(),"u"),u)); CHECK(u == std::numeric_limits<std::uint64_t>::max());
    CHECK(read_int64(s,member(s,s.root(),"trap"),i)); CHECK(i == 9007199254740993LL);
    for (auto k : {"over","negative","frac","exp","negzero"}) { u = 17; CHECK(!read_uint64(s,member(s,s.root(),k),u)); CHECK(u == 17); }
    i = 17; CHECK(!read_int64(s,member(s,s.root(),"u"),i)); CHECK(i == 17);
    auto first = node(s,s.root()).first_child;
    DocumentEdit edit{EditKind::Replace,first,{}, {},"1","3"};
    OwnedProjectSource candidate; CHECK(parse_project_source("42",candidate));
    CHECK(stage_project_edits(s,std::span(&edit,1),candidate).code == Error::AmbiguousKey); CHECK(candidate.bytes() == "42");
    return true;
}
static bool patch_and_moves() {
    OwnedProjectSource s,c;
    CHECK(parse_project_source(R"( { "ui": {"theme":"dark","n":1e9999}, "a": [9007199254740993,{"x":18446744073709551615},-0],"old":true } )",s));
    auto ui = member(s,s.root(),"ui"); auto theme = member(s,ui,"theme");
    auto a = member(s,s.root(),"a"); auto first = node(s,a).first_child;
    auto second = node(s,first).next_sibling; auto third = node(s,second).next_sibling;
    auto old = member(s,s.root(),"old");
    const std::array<DocumentEdit,4> edits = {{
        {EditKind::Replace,theme,{}, {},"\"dark\"","\"light\""},
        {EditKind::Move,third,a,first,"-0",{}},
        {EditKind::Remove,old,{}, {},"true",{}},
        {EditKind::Insert,{},s.root(),{},node(s,s.root()).raw,R"("new":123456789012345678901234567890)"}
    }};
    CHECK(stage_project_edits(s,edits,c));
    // Original key/colon/value prefix spacing is retained; only changed container
    // separators are rebuilt. This expectation is literal, not an encoder oracle.
    CHECK(c.bytes() == R"( {"ui": {"theme":"light","n":1e9999},"a": [-0,9007199254740993,{"x":18446744073709551615}],"new":123456789012345678901234567890} )");
    CHECK(s.bytes().find("\"theme\":\"dark\"") != std::string_view::npos);
    const auto baseline = std::string(c.bytes()); auto baseline_id = c.root();
    auto bad = edits[0]; bad.expected = "\"wrong\"";
    CHECK(stage_project_edits(s,std::span(&bad,1),c).code == Error::ExpectedMismatch); CHECK(c.bytes() == baseline); CHECK(c.root() == baseline_id);
    bad = edits[0]; bad.replacement = "[1,]"; CHECK(!stage_project_edits(s,std::span(&bad,1),c)); CHECK(c.bytes() == baseline);
    auto conflict = edits; conflict[1] = {EditKind::Replace,ui,{}, {},node(s,ui).raw,"null"};
    CHECK(stage_project_edits(s,conflict,c).code == Error::ConflictingEdits); CHECK(c.bytes() == baseline);
    bad = {EditKind::Move,ui,ui,{},node(s,ui).raw,{}}; CHECK(!stage_project_edits(s,std::span(&bad,1),c)); CHECK(c.bytes() == baseline);
    bad = edits[0]; bad.target = member(c,c.root(),"new"); CHECK(stage_project_edits(s,std::span(&bad,1),c).code == Error::InvalidHandle);
    CHECK(stage_project_edits(s,{},c)); CHECK(c.bytes() == s.bytes());
    CHECK(stage_project_edits(s,std::span(edits.data(),1),s)); CHECK(s.bytes().find("\"light\"") != std::string_view::npos);
    return true;
}
static bool structural_edit_cases() {
    OwnedProjectSource s,c; CHECK(parse_project_source(R"({"a":[1,2,3],"b":[],"o":{"x":1e9999},"p":{}})",s));
    auto a=member(s,s.root(),"a"), b=member(s,s.root(),"b"), o=member(s,s.root(),"o"), p=member(s,s.root(),"p");
    auto one=node(s,a).first_child, two=node(s,one).next_sibling, three=node(s,two).next_sibling;
    auto x=member(s,o,"x");
    const std::array<DocumentEdit,5> edits={{{EditKind::Remove,one,{}, {},"1",{}},{EditKind::Remove,two,{}, {},"2",{}},{EditKind::Move,three,b,{},"3",{}},{EditKind::Move,x,p,{},"1e9999",{}},{EditKind::Insert,{},b,{},"[]","-0"}}};
    CHECK(stage_project_edits(s,edits,c)); CHECK(c.bytes() == R"({"a":[],"b":[3,-0],"o":{},"p":{"x":1e9999}})");
    DocumentEdit insert{EditKind::Insert,{},p,{},"{}",R"("x":1)"};
    auto dup=edits; dup[4]=insert; CHECK(stage_project_edits(s,dup,c).code == Error::AmbiguousKey);
    DocumentEdit root{EditKind::Replace,s.root(),{}, {},s.bytes()," [1e9999,-0] "}; CHECK(stage_project_edits(s,std::span(&root,1),c)); CHECK(c.bytes()==" [1e9999,-0] ");
    return true;
}
static bool limits_and_rollback() {
    OwnedProjectSource s; CHECK(parse_project_source("null",s)); const auto id=s.root();
    auto check = [&](std::string_view json, DocumentLimits l, Error error) { auto e=parse_project_source(json,s,l); return e.code==error && s.bytes()=="null" && s.root()==id; };
    DocumentLimits l;
    l.source_bytes=2; CHECK(check("[1]",l,Error::SourceLimit));
    l={}; l.depth=1; CHECK(check("[[]]",l,Error::DepthLimit));
    l={}; l.records=2; CHECK(check("[1,2]",l,Error::RecordLimit));
    l={}; l.tokens=2; CHECK(check("[1]",l,Error::TokenLimit));
    l={}; l.number_bytes=3; CHECK(check("1234",l,Error::TextLimit));
    l={}; l.key_bytes=1; CHECK(check(R"({"ab":1})",l,Error::TextLimit));
    l={}; l.raw_key_bytes=1; CHECK(check(R"({"\u0061":1})",l,Error::TextLimit));
    l={}; l.string_bytes=1; CHECK(check("\"ab\"",l,Error::TextLimit));
    l={}; l.raw_string_bytes=2; CHECK(check(R"("\u0061")",l,Error::TextLimit));
    l={}; l.decoded_bytes=2; CHECK(check(R"({"a":"bc"})",l,Error::TextLimit));
    l={}; l.peak_bytes=s.owned_bytes(); CHECK(check("[]",l,Error::PeakLimit));
    l={}; l.depth=129; CHECK(check("[]",l,Error::InvalidLimits));
    DocumentEdit edit{EditKind::Replace,s.root(),{}, {},"null","[1,2,3]"}; OwnedProjectSource c; CHECK(parse_project_source("42",c));
    l={}; l.candidate_bytes=2; CHECK(stage_project_edits(s,std::span(&edit,1),c,l).code==Error::CandidateLimit); CHECK(c.bytes()=="42");
    l={}; l.edits=0; CHECK(stage_project_edits(s,std::span(&edit,1),c,l).code==Error::EditLimit);
    l={}; l.replacement_bytes=1; CHECK(stage_project_edits(s,std::span(&edit,1),c,l).code==Error::EditLimit);
    l={}; l.peak_bytes=s.owned_bytes()+c.owned_bytes(); CHECK(stage_project_edits(s,std::span(&edit,1),c,l).code==Error::PeakLimit); CHECK(c.bytes()=="42");
    return true;
}
static bool allocation_and_peak() {
    constexpr std::string_view json=R"({"key":"value","n":1e9999})";
    OwnedProjectSource s,out,reference; CHECK(parse_project_source(json,s)); CHECK(parse_project_source("42",out));
    CHECK(parse_project_source(json,reference));
    const auto old=out.root();
    for (int i=0;i<4;++i) {
        fail_after=i; auto d=parse_project_source(json,out); fail_after=-1;
        CHECK(d.code==Error::AllocationFailure); CHECK(out.bytes()=="42"); CHECK(out.root()==old);
    }
    OwnedProjectSource probe; fail_after=4; auto probe_result=parse_project_source(json,probe); const auto parse_remaining=fail_after; fail_after=-1;
    CHECK(probe_result); CHECK(parse_remaining==0);
    DocumentLimits l; l.peak_bytes=out.owned_bytes()+reference.owned_bytes()-1;
    CHECK(parse_project_source(json,out,l).code==Error::PeakLimit); CHECK(out.root()==old);
    ++l.peak_bytes; CHECK(parse_project_source(json,out,l)); CHECK(out.bytes()==json);
    CHECK(parse_project_source("42",out)); const auto new_old=out.root();
    auto key=member(s,s.root(),"key"); DocumentEdit edit{EditKind::Replace,key,{}, {},"\"value\"","\"updated\""};
    for (int i=0;i<6;++i) {
        fail_after=i; auto d=stage_project_edits(s,std::span(&edit,1),out); fail_after=-1;
        CHECK(d.code==Error::AllocationFailure); CHECK(out.bytes()=="42"); CHECK(out.root()==new_old); CHECK(s.bytes()==json);
    }
    fail_after=6; auto staged=stage_project_edits(s,std::span(&edit,1),out); const auto stage_remaining=fail_after; fail_after=-1;
    CHECK(staged); CHECK(stage_remaining==0); CHECK(out.bytes()==R"({"key":"updated","n":1e9999})");
    return true;
}
static bool boundaries_and_opaque_policy() {
    OwnedProjectSource s,c;
    std::string deep(128,'['); deep+="0"; deep.append(128,']'); CHECK(parse_project_source(deep,s));
    deep.insert(0,"["); deep+="]"; CHECK(parse_project_source(deep,s).code==Error::DepthLimit);
    const std::string key(1024,'k'); const std::string exact="{\""+key+"\":1}"; CHECK(parse_project_source(exact,s));
    DocumentEdit insert{EditKind::Insert,{},s.root(),{},s.bytes(),{}};
    // The backing string must outlive the synchronous edit call.
    const std::string oversized="\""+key+"x\":2"; insert.replacement=oversized;
    CHECK(stage_project_edits(s,std::span(&insert,1),c).code==Error::TextLimit); CHECK(c.bytes().empty());
    CHECK(parse_project_source(R"({"x":1e9999,"\u0078":-0,"safe":[9007199254740993]})",s));
    auto safe=member(s,s.root(),"safe"); DocumentEdit e{EditKind::Replace,safe,{}, {},"[9007199254740993]","[18446744073709551615]"};
    CHECK(stage_project_edits(s,std::span(&e,1),c)); CHECK(c.bytes()==R"({"x":1e9999,"\u0078":-0,"safe":[18446744073709551615]})");
    auto ambiguous_anchor=node(s,s.root()).first_child;
    e={EditKind::Insert,{},s.root(),ambiguous_anchor,s.bytes(),R"("new":1)"};
    CHECK(stage_project_edits(s,std::span(&e,1),c).code==Error::AmbiguousKey);
    CHECK(parse_project_source(R"({"a":{"x":1},"\u0061":{"x":2}})",s));
    auto a=node(s,s.root()).first_child; auto child=node(s,a).first_child;
    e={EditKind::Replace,child,{}, {},"1","3"}; CHECK(stage_project_edits(s,std::span(&e,1),c).code==Error::AmbiguousKey);
    return true;
}
static bool fixed_width_boundaries_and_batch_refusal() {
    OwnedProjectSource s,c;
    constexpr std::array<std::string_view,7> raw={"0","9007199254740991","9007199254740992","9007199254740993","-9007199254740991","-9007199254740992","-9007199254740993"};
    constexpr std::array<std::int64_t,7> expected={0,9007199254740991LL,9007199254740992LL,9007199254740993LL,-9007199254740991LL,-9007199254740992LL,-9007199254740993LL};
    for(std::size_t i=0;i<raw.size();++i) { CHECK(parse_project_source(raw[i],s)); std::int64_t n=99; CHECK(read_int64(s,s.root(),n)); CHECK(n==expected[i]); CHECK(s.bytes()==raw[i]); }
    for(auto invalid:{"9223372036854775808","-9223372036854775809","123456789012345678901234567890","1e9999","-0"}) {
        CHECK(parse_project_source(invalid,s)); std::int64_t n=99; CHECK(!read_int64(s,s.root(),n)); CHECK(n==99);
    }
    CHECK(parse_project_source(R"({"a":[1,2],"b":[],"x":0})",s)); CHECK(parse_project_source("false",c));
    auto a=member(s,s.root(),"a"), b=member(s,s.root(),"b"), x=member(s,s.root(),"x");
    auto one=node(s,a).first_child, two=node(s,one).next_sibling;
    std::array<DocumentEdit,2> batch={{{EditKind::Insert,{},b,{},"[]","1e9999"},{EditKind::Replace,x,{}, {},"wrong","9"}}};
    const auto baseline=c.root(); CHECK(stage_project_edits(s,batch,c).code==Error::ExpectedMismatch); CHECK(c.bytes()=="false"); CHECK(c.root()==baseline);
    batch={{{EditKind::Move,one,a,two,"1",{}},{EditKind::Replace,two,{}, {},"2","3"}}}; CHECK(stage_project_edits(s,batch,c)); CHECK(c.bytes()==R"({"a":[1,3],"b":[],"x":0})");
    batch={{{EditKind::Move,a,b,{},"[1,2]",{}},{EditKind::Replace,one,{}, {},"1","9"}}}; CHECK(stage_project_edits(s,batch,c).code==Error::InvalidEdit); // Object member cannot move to an array.
    batch={{{EditKind::Remove,a,{}, {},"[1,2]",{}},{EditKind::Replace,one,{}, {},"1","9"}}}; CHECK(stage_project_edits(s,batch,c).code==Error::ConflictingEdits);
    DocumentEdit malformed{EditKind::Insert,{},b,{0,1},"[]","3"}; CHECK(stage_project_edits(s,std::span(&malformed,1),c).code==Error::InvalidHandle);
    return true;
}
static bool bounded_profile_strings() {
    constexpr std::size_t profile_limit=1024u*1024u;
    OwnedProjectSource source,candidate;
    const std::string decoded(profile_limit,'p');
    const std::string exact="\""+decoded+"\"";
    CHECK(parse_project_source(exact,source));
    CHECK(node(source,source.root()).decoded==decoded);
    const auto baseline=source.root();
    const std::string oversized="\""+decoded+"p\"";
    CHECK(parse_project_source(oversized,source).code==Error::TextLimit);
    CHECK(source.root()==baseline); CHECK(source.bytes()==exact);
    // Maximum JSON escaping must fit too; raw and decoded ceilings differ.
    std::string escaped; escaped.reserve(6u*profile_limit+2u); escaped+='"';
    for(std::size_t i=0;i<profile_limit;++i) escaped+="\\u0070";
    escaped+='"';
    CHECK(parse_project_source(escaped,candidate));
    CHECK(node(candidate,candidate.root()).decoded==decoded);
    CHECK(candidate.bytes()==escaped);
    OwnedProjectSource project,updated;
    CHECK(parse_project_source(R"({"profile_json":"","future":1e9999})",project));
    const auto profile=member(project,project.root(),"profile_json");
    DocumentEdit edit{EditKind::Replace,profile,{},{},"\"\"",exact};
    CHECK(stage_project_edits(project,std::span(&edit,1),updated));
    CHECK(node(updated,member(updated,updated.root(),"profile_json")).decoded==decoded);
    CHECK(node(updated,member(updated,updated.root(),"future")).raw=="1e9999");
    return true;
}
int main() {
    const std::array tests={ownership_and_lexemes,grammar_and_unicode,duplicates_and_integers,patch_and_moves,structural_edit_cases,limits_and_rollback,allocation_and_peak,boundaries_and_opaque_policy,fixed_width_boundaries_and_batch_refusal,bounded_profile_strings};
    int failed=0; for(std::size_t i=0;i<tests.size();++i) { bool ok=tests[i](); std::printf("case %zu %s\n",i+1,ok?"PASS":"FAIL"); failed+=!ok; }
    std::printf("project_source: %zu cases, %d failures\n",tests.size(),failed); return failed?1:0;
}
