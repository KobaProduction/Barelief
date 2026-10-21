#include <desktop/display_refresh_rates.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <string>
using namespace desktop;
namespace {
void require(bool value,const char* why){if(!value){std::fprintf(stderr,"FAIL %s\n",why);std::exit(1);}}
void filtering_tests(){
    const DisplayModeSnapshot current{2560,1440,32,0,false,165};
    const std::array<DisplayModeSnapshot,11> modes{{
        {2560,1440,32,0,false,144},{2560,1440,32,0,false,60},
        {2560,1440,32,0,false,144},{1920,1080,32,0,false,240},
        {2560,1440,16,0,false,75},{2560,1440,32,1,false,120},
        {2560,1440,32,0,true,90},{2560,1440,32,0,false,0},
        {2560,1440,32,0,false,1},{2560,1440,32,0,false,-30},
        {2560,1440,32,0,false,std::numeric_limits<double>::quiet_NaN()}}};
    const auto rates=refresh_rates_for_modes(current,modes);
    require(rates.current==165,"current native OS refresh is preserved");
    require(rates.supported==std::vector<double>{60,144,165},"only current geometry modes; sorted deduplicated; current included");
    const auto singleton=refresh_rates_for_modes(current,{});
    require(singleton.supported==std::vector<double>{165},"missing driver catalog does not fabricate alternatives");
    auto rotated=current;rotated.orientation=1;rotated.refresh_rate=120;
    const auto rotation=refresh_rates_for_modes(rotated,modes);
    require(rotation.supported==std::vector<double>{120},"current rotation filters alternatives");
    auto interlaced=current;interlaced.interlaced=true;interlaced.refresh_rate=90;
    require(refresh_rates_for_modes(interlaced,modes).supported==std::vector<double>{90},"scan mode preserved");
    auto invalid=current;invalid.width=0;require(refresh_rates_for_modes(invalid,modes).supported.empty(),"unknown resolution refuses catalog");
    auto no_current=current;no_current.refresh_rate=0;const auto unknown=refresh_rates_for_modes(no_current,modes);
    require(unknown.current==0 && unknown.supported==std::vector<double>{60,144},"unknown current frequency stays unknown");
    const std::array<DisplayModeSnapshot,2> fractions{{{2560,1440,32,0,false,59.94},{2560,1440,32,0,false,60}}};
    require(refresh_rates_for_modes(current,fractions).supported==std::vector<double>{59.94,60,165},"distinct fractional catalog values remain distinct");
}
void native_probe(){
#ifdef _WIN32
    // Read-only query: no GUI, focus changes, screenshots or mode mutation.
    const auto native=supported_screen_refresh_rates({});
    const auto api=native_display_mode_api();const auto current=api.query({});
    if(current){const auto via_api=refresh_rates_for_modes(*current,api.enumerate({}));require(via_api.current==native.current && via_api.supported==native.supported,"injected native query/enumeration boundary matches the read-only catalog API");}
    std::printf("native-current=%.6f supported=",native.current);for(double value:native.supported)std::printf(" %.6f",value);std::puts("");
    require(std::is_sorted(native.supported.begin(),native.supported.end()),"native rates sorted");
    require(std::adjacent_find(native.supported.begin(),native.supported.end())==native.supported.end(),"native rates unique");
    for(double value:native.supported)require(std::isfinite(value) && value>1,"native rates usable");
    if(native.current>1)require(std::find(native.supported.begin(),native.supported.end(),native.current)!=native.supported.end(),"native current present");
    else std::puts("native read-only probe unavailable: no active display mode reported");
    const auto absent=supported_screen_refresh_rates(L"\\\\.\\DISPLAY_INVALID_BARELIEF_TEST");
    require(absent.supported.empty() && absent.current==0,"invalid native display never falls back to another monitor");
    std::wstring embedded=L"\\\\.\\DISPLAY1";embedded.push_back(L'\0');embedded.append(L"ignored");
    require(supported_screen_refresh_rates(embedded).supported.empty(),"embedded-zero device is refused");
#endif
}
void session_selection(DisplayModeApi& fake,std::map<std::wstring,DisplayModeSnapshot>& displays,
 std::vector<std::pair<std::wstring,double>>& applied,unsigned& enumerations) {
    {
        DisplayRefreshSession session(fake);
        require(session.select_maximum(L"A") && displays[L"A"].refresh_rate==165,"Maximum applies the highest supported mode through the injected boundary");
        require(enumerations==1,"Maximum uses one shared mode catalog for selection and validation");
        require(session.select_maximum(L"A") && enumerations==1,"unchanged Maximum reuses its catalog without another enumeration");
        require(session.select(L"A",120) && displays[L"A"].refresh_rate==120,"supported explicit frequency switches mode");
        const auto calls=applied.size();
        require(!session.select(L"A",240) && applied.size()==calls,"unsupported rate never reaches the OS boundary");
        require(session.select(L"A",120) && applied.size()==calls,"already-current rate causes no redundant mode change");
        const auto knownCatalogs=enumerations;
        require(session.select_or_maximum(L"A",120) && enumerations==knownCatalogs,"already-current physical policy does not enumerate");
        require(session.release(L"A") && displays[L"A"].refresh_rate==60,"release restores the original rate across repeated selections");
        require(session.select_maximum(L"A") && session.release(L"A") && session.select_maximum(L"B"),"monitor transfer restores the previous monitor before applying the next");
        require(displays[L"A"].refresh_rate==60 && displays[L"B"].refresh_rate==144,"monitor sessions remain independent");
    }
}
void session_restore_failures(DisplayModeApi& fake,std::map<std::wstring,DisplayModeSnapshot>& displays,
 std::vector<std::pair<std::wstring,double>>& applied,bool& reject) {
    {
        DisplayRefreshSession session(fake);reject=true;const auto before=applied.size();
        require(!session.select_maximum(L"A") && displays[L"A"].refresh_rate==60,"rejected OS switch reports failure without changing current mode");
        reject=false;session.restore_all();require(applied.size()==before+1,"failed apply never creates a restoration claim");
        require(session.select_maximum(L"A"),"prepare external-mode-change fixture");
        displays[L"A"].refresh_rate=120;const auto calls=applied.size();session.restore_all();
        require(applied.size()==calls && displays[L"A"].refresh_rate==120,"teardown preserves externally changed display settings");
        require(session.select_maximum(L"A") && session.release(L"A") && displays[L"A"].refresh_rate==120,"a fresh claim restores the latest externally selected original mode");
        require(session.select(L"A",60),"prepare failed restoration fixture");reject=true;
        require(!session.release(L"A"),"restoration failure is observable");reject=false;
        require(session.release(L"A") && displays[L"A"].refresh_rate==120,"failed restoration remains retryable");
        require(session.select_maximum(L"A"),"prepare detached display fixture");displays.erase(L"A");
        const auto beforeDetached=applied.size();require(!session.release(L"A") && applied.size()==beforeDetached,"detached or temporarily unavailable display retains a retryable claim without targeting another monitor");
        require(!session.select_maximum(L"") && !session.select(L"missing",60),"mode mutation requires a known named monitor");
    }}
void uncertain_readback(DisplayRefreshSession& session,DisplayModeSnapshot& actual,
 unsigned& missing_reads,unsigned& missing_after_apply,std::vector<double>& applied) {
missing_after_apply=1;
        require(!session.select_maximum(L"A") && actual.refresh_rate==60 && applied==std::vector<double>{165,60},"unavailable post-apply query rolls back before reporting failure");
        const auto calls=applied.size();session.restore_all();require(applied.size()==calls,"confirmed rollback leaves no new restoration claim");
        missing_after_apply=2;
        require(!session.select_maximum(L"A") && actual.refresh_rate==165,"repeated unavailable verification retains a claim without blindly overwriting an unknown current mode");
        missing_reads=1;require(!session.reconcile(L"A"),"deferred reconciliation can retry a temporarily missing readback");
        require(session.reconcile(L"A") && actual.refresh_rate==60,"deferred reconciliation restores the old policy mode");
        missing_after_apply=0;require(session.select(L"A",120),"prepare an already owned previous policy");
        missing_after_apply=2;require(!session.select(L"A",165) && actual.refresh_rate==165,"later uncertain request remains pending");
        require(session.reconcile(L"A") && actual.refresh_rate==120,"deferred reconciliation restores the previous policy rather than the session original");
        require(session.release(L"A") && actual.refresh_rate==60,"reconciliation retains the original session restoration boundary");
        missing_after_apply=2;require(!session.select(L"A",165),"prepare external change during pending reconciliation");
        actual.refresh_rate=120;const auto beforeExternal=applied.size();
        require(session.reconcile(L"A") && applied.size()==beforeExternal && actual.refresh_rate==120,"deferred reconciliation preserves an intervening external mode");
}
void failed_rollback(DisplayRefreshSession& session,DisplayModeSnapshot& actual,unsigned& missing_reads,
 unsigned& missing_after_apply,std::vector<double>& applied,bool& reject_restore,bool& external_change) {
        actual.refresh_rate=60;
        missing_after_apply=1;reject_restore=true;
        require(!session.select_maximum(L"A") && actual.refresh_rate==165,"failed rollback reports failure and retains the changed mode's claim");
        missing_reads=1;require(!session.release(L"A"),"missing restoration query must retain ownership for a later retry");
        reject_restore=false;require(session.release(L"A") && actual.refresh_rate==60,"failed post-apply rollback remains restorable");
        missing_after_apply=0;require(session.select(L"A",120),"prepare prior owned mode fixture");
        missing_after_apply=1;require(!session.select(L"A",165) && actual.refresh_rate==120,"failed later request restores the previous owned session rate");
        require(session.release(L"A") && actual.refresh_rate==60,"rollback of a later request preserves the first original mode");
        missing_after_apply=0;external_change=true;const auto before=applied.size();
        require(!session.select(L"A",165) && actual.refresh_rate==120,"unexpected valid readback reports failure");
        session.restore_all();require(applied.size()==before+1 && actual.refresh_rate==120,"an intervening external mode is never rolled back or claimed");}
void session_tests(){
    std::map<std::wstring,DisplayModeSnapshot> displays{{L"A",{2560,1440,32,0,false,60}},{L"B",{1920,1080,32,0,false,75}}};
    std::vector<std::pair<std::wstring,double>> applied;bool reject=false;unsigned enumerations=0;
    DisplayModeApi fake{
        [&](std::wstring_view device)->std::optional<DisplayModeSnapshot>{const auto it=displays.find(std::wstring(device));return it==displays.end()?std::nullopt:std::optional{it->second};},
        [&](std::wstring_view device){++enumerations;std::vector<DisplayModeSnapshot> result;const auto it=displays.find(std::wstring(device));if(it!=displays.end())for(const double rate:device==L"A"?std::vector<double>{60,120,165}:std::vector<double>{75,144}){auto mode=it->second;mode.refresh_rate=rate;result.push_back(mode);}return result;},
        [&](std::wstring_view device,double rate){applied.emplace_back(device,rate);if(reject)return false;displays.at(std::wstring(device)).refresh_rate=rate;return true;}};
    session_selection(fake,displays,applied,enumerations);
    require(displays[L"B"].refresh_rate==75,"teardown restores the second monitor's original mode");
    session_restore_failures(fake,displays,applied,reject);
}
void verification_failure_tests(){
    DisplayModeSnapshot actual{2560,1440,32,0,false,60};
    unsigned missing_reads=0,missing_after_apply=0;bool reject_restore=false,external_change=false;
    std::vector<double> applied;
    DisplayModeApi fake{
        [&](std::wstring_view)->std::optional<DisplayModeSnapshot>{if(missing_reads){--missing_reads;return {};}return actual;},
        [&](std::wstring_view){auto a=actual,b=actual,c=actual;a.refresh_rate=60;b.refresh_rate=120;c.refresh_rate=165;return std::vector{a,b,c};},
        [&](std::wstring_view,double rate){applied.push_back(rate);if(reject_restore && rate==60)return false;actual.refresh_rate=external_change && rate==165?120:rate;if(rate==165)missing_reads=missing_after_apply;return true;}};
    {
        DisplayRefreshSession session(fake);uncertain_readback(session,actual,missing_reads,missing_after_apply,applied);
        failed_rollback(session,actual,missing_reads,missing_after_apply,applied,reject_restore,external_change);
    }
    require(display_device_for_monitor(0).empty(),"invalid native monitor does not resolve to a different display");
}
void capability_change_tests(){
    DisplayModeSnapshot actual{2560,1440,32,0,false,60};double maximum=165;unsigned catalogs=0;bool unavailable=false;
    DisplayModeApi fake{
        [&](std::wstring_view)->std::optional<DisplayModeSnapshot>{return unavailable?std::nullopt:std::optional{actual};},
        [&](std::wstring_view){++catalogs;auto a=actual,b=actual,c=actual;a.refresh_rate=60;b.refresh_rate=120;c.refresh_rate=maximum;return std::vector{a,b,c};},
        [&](std::wstring_view,double rate){if(rate>maximum)return false;actual.refresh_rate=rate;return true;}};
    DisplayRefreshSession session(fake);
    require(session.select_maximum(L"A") && catalogs==1 && actual.refresh_rate==165,"initial Maximum uses one catalog");
    require(session.select_maximum(L"A") && catalogs==1,"unchanged Maximum does not enumerate");
    maximum=240;session.invalidate_catalog(L"A");
    require(session.select_maximum(L"A") && catalogs==2 && actual.refresh_rate==240,"topology invalidation discovers expanded same-geometry catalog");
    maximum=144;actual.refresh_rate=120;session.invalidate_catalog(L"A");
    require(session.select_maximum(L"A") && catalogs==3 && actual.refresh_rate==144,"shrinking same-geometry catalog selects its new maximum");
    require(session.release(L"A") && actual.refresh_rate==120,"release preserves externally established original mode");
    maximum=165;require(session.select_maximum(L"A") && catalogs==4 && actual.refresh_rate==165,"reacquiring a released monitor refreshes its catalog");
    unavailable=true;require(!session.select_maximum(L"A"),"disconnected query invalidates the capability snapshot");
    unavailable=false;maximum=240;
    require(session.select_maximum(L"A") && catalogs==5 && actual.refresh_rate==240,"reconnected monitor discovers new capabilities");
}
}
int main(){filtering_tests();session_tests();verification_failure_tests();capability_change_tests();native_probe();std::puts("display refresh catalog filtering, injected session switching/restoration/rollback and read-only native probe PASS");}
