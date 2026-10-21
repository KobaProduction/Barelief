#pragma once
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace desktop {
struct DisplayRefreshRates {
    std::vector<double> supported;
    double current=0; // Zero means that the OS did not provide a usable rate.
};
// Platform-neutral snapshot used by the adapter's filtering rule and tests.
struct DisplayModeSnapshot {
    std::uint32_t width=0,height=0,bits_per_pixel=0,orientation=0;
    bool interlaced=false;
    double refresh_rate=0;
};
// Same physical resolution, depth, orientation and scan mode as current.
// Sorted and deduplicated; includes the current OS mode even when the driver
// omits it from its enumerated catalog. No suggested/default frequencies.
DisplayRefreshRates refresh_rates_for_modes(const DisplayModeSnapshot& current,
    std::span<const DisplayModeSnapshot> modes);
// Read-only. Windows device name is DISPLAY_DEVICEW::DeviceName, e.g.
// L"\\\\.\\DISPLAY1". A presentable screen name is not a native device identity.
// Empty means the current display.
// Failed/unsupported queries return an empty catalog and current=0.
DisplayRefreshRates supported_screen_refresh_rates(std::wstring_view display_device);
// Native monitor handle -> GDI display identity. This adapter does not depend
// on the GUI toolkit that supplied the handle. Invalid handles return empty.
std::wstring display_device_for_monitor(std::uintptr_t native_monitor);

// Injected OS boundary. apply changes only refresh rate in the current mode,
// without writing persistent display settings. Tests supply an in-memory API.
struct DisplayModeApi {
    std::function<std::optional<DisplayModeSnapshot>(std::wstring_view)> query;
    std::function<std::vector<DisplayModeSnapshot>(std::wstring_view)> enumerate;
    std::function<bool(std::wstring_view,double)> apply;
};
DisplayModeApi native_display_mode_api();
class DisplayRefreshSession final {
public:
    explicit DisplayRefreshSession(DisplayModeApi api=native_display_mode_api());
    ~DisplayRefreshSession();
    DisplayRefreshSession(const DisplayRefreshSession&)=delete;
    DisplayRefreshSession& operator=(const DisplayRefreshSession&)=delete;
    // Success requires readback of the requested mode. Missing readback attempts
    // rollback; failed/uncertain rollback remains retryable through release.
    // An unexpected valid mode is preserved as an external/unattributed change.
    bool select(std::wstring_view device,double refresh_rate);
    bool select_maximum(std::wstring_view device);
    bool select_or_maximum(std::wstring_view device,double refresh_rate);
    // Complete only an uncertain failed request: restore its previous policy
    // mode when still owned, while preserving intervening external changes.
    bool reconcile(std::wstring_view device);
    // Monitor/topology notifications invalidate capability snapshots. An empty
    // identity invalidates all displays; no OS query or mutation occurs here.
    void invalidate_catalog(std::wstring_view device={});
    // False can mean a temporarily unavailable query or failed restoration.
    // The caller should retain the device for retry even after select fails.
    bool release(std::wstring_view device);
    void restore_all();
private:
    struct Change {DisplayModeSnapshot original,last;std::optional<DisplayModeSnapshot> rollback_previous;};
    using ChangeIterator=std::map<std::wstring,Change>::iterator;
    ChangeIterator reconcile_selection_claim(const std::wstring&,const DisplayModeSnapshot&);
    std::optional<bool> rollback_unverified_selection(std::wstring_view,const DisplayModeSnapshot&,const DisplayModeSnapshot&);
    bool record_verified_selection(const std::wstring&,const DisplayModeSnapshot&,const DisplayModeSnapshot&,const DisplayModeSnapshot&,ChangeIterator);
    void retain_uncertain_selection(const std::wstring&,const DisplayModeSnapshot&,const DisplayModeSnapshot&,ChangeIterator);
    std::optional<bool> select_cached_maximum(std::wstring_view,std::optional<DisplayModeSnapshot>&);
    std::optional<bool> reconcile_previous_selection(std::wstring_view,ChangeIterator,const DisplayModeSnapshot&,const DisplayModeSnapshot&);
    bool finish_reconciled_selection(ChangeIterator,const DisplayModeSnapshot&);
    bool recover_unverified_selection(std::wstring_view,const DisplayModeSnapshot&,const DisplayModeSnapshot&,ChangeIterator);
    bool select_catalog_rate(std::wstring_view,const DisplayModeSnapshot&,double,bool);
    std::optional<bool> restore_previous_selection(std::wstring_view,ChangeIterator,const DisplayModeSnapshot&);
    bool restore_original_selection(std::wstring_view,ChangeIterator,const DisplayModeSnapshot&);
    DisplayModeApi api_;
    std::map<std::wstring,Change> changes_;
    struct Maximum {DisplayModeSnapshot geometry; double rate=0;};
    std::map<std::wstring,Maximum> maximum_;
    bool select_current(std::wstring_view,const DisplayModeSnapshot&,double);
    bool select_policy(std::wstring_view,double,bool);
};
}
