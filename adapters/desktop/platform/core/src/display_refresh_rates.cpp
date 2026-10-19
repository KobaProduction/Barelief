#include <desktop/display_refresh_rates.h>
#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace desktop {
namespace {
bool usable_rate(double rate){return std::isfinite(rate) && rate>1;}
bool same_geometry(const DisplayModeSnapshot& left,const DisplayModeSnapshot& right){
    return left.width==right.width && left.height==right.height &&
        left.bits_per_pixel==right.bits_per_pixel && left.orientation==right.orientation &&
        left.interlaced==right.interlaced;
}
#ifdef _WIN32
std::optional<DisplayModeSnapshot> snapshot(const DEVMODEW& mode,const DisplayModeSnapshot* current=nullptr){
    if(!mode.dmPelsWidth || !mode.dmPelsHeight || !usable_rate(mode.dmDisplayFrequency))return {};
    // Some drivers omit optional dmFields bits in the enumerated modes. Their
    // absence does not make the resolution/frequency pair unsupported.
    return DisplayModeSnapshot{mode.dmPelsWidth,mode.dmPelsHeight,mode.dmBitsPerPel,
        mode.dmFields&DM_DISPLAYORIENTATION?mode.dmDisplayOrientation:current?current->orientation:0,
        mode.dmFields&DM_DISPLAYFLAGS?bool(mode.dmDisplayFlags&DM_INTERLACED):current?current->interlaced:false,double(mode.dmDisplayFrequency)};
}
bool device_name_valid(std::wstring_view name){return name.find(L'\0')==std::wstring_view::npos;}
std::optional<DEVMODEW> current_mode(std::wstring_view display_device){
    if(!device_name_valid(display_device))return {};
    const std::wstring name(display_device);DEVMODEW mode{};mode.dmSize=sizeof(mode);
    if(!EnumDisplaySettingsExW(name.empty()?nullptr:name.c_str(),ENUM_CURRENT_SETTINGS,&mode,0))return {};
    return mode;
}
std::vector<DisplayModeSnapshot> enumerated_modes(std::wstring_view display_device,const DisplayModeSnapshot& current){
    std::vector<DisplayModeSnapshot> catalog;
    if(!device_name_valid(display_device))return catalog;
    const std::wstring name(display_device);
    // No EDS_RAWMODE or EDS_ROTATEDMODE: retain only attached monitor modes.
    for(DWORD index=0;index<ENUM_REGISTRY_SETTINGS;++index){
        DEVMODEW mode{};mode.dmSize=sizeof(mode);
        if(!EnumDisplaySettingsExW(name.empty()?nullptr:name.c_str(),index,&mode,0))break;
        if(auto value=snapshot(mode,&current)){
            if(!value->bits_per_pixel)value->bits_per_pixel=current.bits_per_pixel;
            catalog.push_back(*value);
        }
    }
    return catalog;
}
#endif
bool same_mode(const DisplayModeSnapshot& a,const DisplayModeSnapshot& b){return same_geometry(a,b) && std::abs(a.refresh_rate-b.refresh_rate)<1e-6;}
}
DisplayRefreshRates refresh_rates_for_modes(const DisplayModeSnapshot& current,std::span<const DisplayModeSnapshot> modes){
    DisplayRefreshRates out;
    if(!current.width || !current.height || !current.bits_per_pixel)return out;
    if(usable_rate(current.refresh_rate)){out.current=current.refresh_rate;out.supported.push_back(out.current);}
    for(const auto& mode:modes)if(same_geometry(mode,current) && usable_rate(mode.refresh_rate))out.supported.push_back(mode.refresh_rate);
    std::sort(out.supported.begin(),out.supported.end());
    out.supported.erase(std::unique(out.supported.begin(),out.supported.end()),out.supported.end());
    return out;
}
DisplayRefreshRates supported_screen_refresh_rates(std::wstring_view display_device){
#ifdef _WIN32
    const auto current=current_mode(display_device);if(!current)return {};
    const auto actual=snapshot(*current);if(!actual)return {};
    return refresh_rates_for_modes(*actual,enumerated_modes(display_device,*actual));
#else
    (void)display_device;return {};
#endif
}
std::wstring display_device_for_monitor(std::uintptr_t native_monitor){
#ifdef _WIN32
    if(!native_monitor)return {};
    MONITORINFOEXW info{};info.cbSize=sizeof(info);
    if(!GetMonitorInfoW(reinterpret_cast<HMONITOR>(native_monitor),reinterpret_cast<MONITORINFO*>(&info)))return {};
    return info.szDevice;
#else
    (void)native_monitor;return {};
#endif
}
DisplayModeApi native_display_mode_api(){
#ifdef _WIN32
    return {
        [](std::wstring_view device)->std::optional<DisplayModeSnapshot>{const auto mode=current_mode(device);return mode?snapshot(*mode):std::nullopt;},
        [](std::wstring_view device){const auto mode=current_mode(device);const auto actual=mode?snapshot(*mode):std::nullopt;return actual?enumerated_modes(device,*actual):std::vector<DisplayModeSnapshot>{};},
        [](std::wstring_view device,double rate){
            if(device.empty() || !usable_rate(rate))return false;
            const auto before=current_mode(device);if(!before)return false;
            const auto actual=snapshot(*before);if(!actual)return false;
            const auto rates=refresh_rates_for_modes(*actual,enumerated_modes(device,*actual));
            if(std::none_of(rates.supported.begin(),rates.supported.end(),[rate](double supported){return std::abs(rate-supported)<1e-6;}))return false;
            auto requested=*before;requested.dmFields=DM_DISPLAYFREQUENCY;requested.dmDisplayFrequency=DWORD(std::round(rate));
            const std::wstring name(device);
            if(ChangeDisplaySettingsExW(name.c_str(),&requested,nullptr,CDS_TEST,nullptr)!=DISP_CHANGE_SUCCESSFUL)return false;
            return ChangeDisplaySettingsExW(name.c_str(),&requested,nullptr,0,nullptr)==DISP_CHANGE_SUCCESSFUL;
        }};
#else
    return {};
#endif
}
DisplayRefreshSession::DisplayRefreshSession(DisplayModeApi api):api_(std::move(api)){}
DisplayRefreshSession::~DisplayRefreshSession(){try{restore_all();}catch(...){}}
bool DisplayRefreshSession::select(std::wstring_view device,double rate){
    if(device.empty() || device.find(L'\0')!=std::wstring_view::npos || !usable_rate(rate) || !api_.query || !api_.enumerate || !api_.apply)return false;
    const auto before=api_.query(device);if(!before){invalidate_catalog(device);return false;}
    if(std::abs(before->refresh_rate-rate)<1e-6)return select_current(device,*before,rate);
    const auto rates=refresh_rates_for_modes(*before,api_.enumerate(device));
    if(std::none_of(rates.supported.begin(),rates.supported.end(),[rate](double value){return std::abs(value-rate)<1e-6;}))return false;
    return select_current(device,*before,rate);
}
bool DisplayRefreshSession::select_current(std::wstring_view device,const DisplayModeSnapshot& current,double rate){
    const auto* before=&current;
    const std::wstring key(device);
    auto existing=changes_.find(key);
    if(existing!=changes_.end() && (same_mode(existing->second.original,*before) ||
       (!same_mode(existing->second.last,*before) &&
        (!existing->second.rollback_previous || !same_mode(*existing->second.rollback_previous,*before))))){changes_.erase(existing);existing=changes_.end();}
    if(std::abs(before->refresh_rate-rate)<1e-6)return true;
    if(!api_.apply(device,rate))return false;
    auto expected=*before;expected.refresh_rate=rate;
    const auto after=api_.query(device);
    if(!after){
        invalidate_catalog(device);
        // Retry readback before rollback: an unrelated external mode must not
        // be overwritten just because the first verification was unavailable.
        const auto retry=api_.query(device);
        if(retry && same_mode(*retry,*before))return false;
        if(retry && !same_mode(*retry,expected)){changes_.erase(key);return false;}
        if(retry){
            api_.apply(device,before->refresh_rate);
            const auto restored=api_.query(device);
            if(restored && same_mode(*restored,*before))return false;
            if(restored && !same_mode(*restored,expected)){changes_.erase(key);return false;}
        }
        if(existing==changes_.end())changes_.emplace(key,Change{*before,expected,*before});
        else {existing->second.last=expected;existing->second.rollback_previous=*before;}
        return false;
    }
    if(!same_mode(expected,*after)){
        // A valid, unexpected mode might be an intervening external change.
        // There is no atomic OS ownership token: preserve it rather than
        // claiming or overwriting a mode this request did not establish.
        if(!same_mode(*before,*after))changes_.erase(key);
        return false;
    }
    if(existing==changes_.end())changes_.emplace(key,Change{*before,expected});
    else {existing->second.last=expected;existing->second.rollback_previous.reset();}
    return true;
}
bool DisplayRefreshSession::select_maximum(std::wstring_view device){
    return select_policy(device,0,true);
}
bool DisplayRefreshSession::select_or_maximum(std::wstring_view device,double rate){
    if(!usable_rate(rate))return false;
    return select_policy(device,rate,false);
}
bool DisplayRefreshSession::select_policy(std::wstring_view device,double rate,bool maximum){
    if(device.empty() || device.find(L'\0')!=std::wstring_view::npos || !api_.query || !api_.enumerate || !api_.apply)return false;
    auto current=api_.query(device);if(!current){invalidate_catalog(device);return false;}
    const std::wstring key(device);
    const auto cached=maximum_.find(key);
    if(maximum && cached!=maximum_.end() && same_geometry(cached->second.geometry,*current) &&
       std::abs(cached->second.rate-current->refresh_rate)<1e-6){
        if(select_current(device,*current,cached->second.rate))return true;
        // One fresh capability read after a failed cached selection.
        invalidate_catalog(device);current=api_.query(device);if(!current)return false;
    }
    if(!maximum && std::abs(current->refresh_rate-rate)<1e-6)return select_current(device,*current,rate);
    const auto rates=refresh_rates_for_modes(*current,api_.enumerate(device));
    if(rates.supported.empty())return false;
    maximum_[key]={*current,rates.supported.back()};
    const bool supported=std::any_of(rates.supported.begin(),rates.supported.end(),[rate](double value){return std::abs(value-rate)<1e-6;});
    return select_current(device,*current,maximum || !supported?rates.supported.back():rate);
}
bool DisplayRefreshSession::reconcile(std::wstring_view device){
    const auto entry=changes_.find(std::wstring(device));
    if(entry==changes_.end() || !entry->second.rollback_previous)return true;
    const auto current=api_.query?api_.query(device):std::nullopt;if(!current){invalidate_catalog(device);return false;}
    const auto previous=*entry->second.rollback_previous;
    if(!same_mode(*current,previous)){
        if(!same_mode(*current,entry->second.last)){changes_.erase(entry);return true;}
        if(!api_.apply || !api_.apply(device,previous.refresh_rate))return false;
        const auto restored=api_.query(device);if(!restored)return false;
        if(!same_mode(*restored,previous)){
            if(!same_mode(*restored,entry->second.last)){changes_.erase(entry);return true;}
            return false;
        }
    }
    if(same_mode(previous,entry->second.original))changes_.erase(entry);
    else {entry->second.last=previous;entry->second.rollback_previous.reset();}
    return true;
}
bool DisplayRefreshSession::release(std::wstring_view device){
    invalidate_catalog(device);
    const auto entry=changes_.find(std::wstring(device));if(entry==changes_.end())return true;
    const auto current=api_.query?api_.query(device):std::nullopt;
    // An unavailable query can be transient or a detached display. Retain a
    // retryable claim, and never redirect restoration to a different monitor.
    if(!current)return false;
    if(same_mode(*current,entry->second.original)){changes_.erase(entry);return true;}
    if(!same_mode(*current,entry->second.last) &&
       (!entry->second.rollback_previous || !same_mode(*current,*entry->second.rollback_previous))){changes_.erase(entry);return true;}
    if(!api_.apply || !api_.apply(device,entry->second.original.refresh_rate))return false;
    const auto restored=api_.query(device);
    if(!restored)return false;
    if(!same_mode(*restored,entry->second.original)){
        if(!same_mode(*restored,*current))changes_.erase(entry);
        return false;
    }
    changes_.erase(entry);return true;
}
void DisplayRefreshSession::invalidate_catalog(std::wstring_view device){
    if(device.empty())maximum_.clear();else maximum_.erase(std::wstring(device));
}
void DisplayRefreshSession::restore_all(){
    std::vector<std::wstring> devices;for(const auto& [device,change]:changes_)devices.push_back(device);
    for(const auto& device:devices)release(device);
}
}
