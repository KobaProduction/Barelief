# These stylesheet properties are mutable. Publish real state changes instead
# of claiming CONSTANT merely to satisfy the meta-object property checks.
include("${CMAKE_CURRENT_LIST_DIR}/patch_dependency_source.cmake")
set(ads_headers "${ADS_SOURCE}/src")
# Q_ENUM_NS exposes this meta-object to consumers of the shared ADS library.
patch_dependency_source("${ads_headers}" ads_globals.h
    "Q_NAMESPACE\n" "Q_NAMESPACE_EXPORT(ADS_EXPORT)\n")
patch_dependency_source("${ads_headers}" ads_globals.h
    "Q_ENUMS(SideBarLocation)" "Q_ENUM_NS(SideBarLocation)")
foreach(property IN ITEMS
        "int sideBarLocation READ sideBarLocation"
        "Qt::Orientation orientation READ orientation"
        "bool activeTab READ isActiveTab"
        "bool iconOnly READ iconOnly")
    patch_dependency_source("${ads_headers}" AutoHideTab.h
        "Q_PROPERTY(${property})" "Q_PROPERTY(${property} NOTIFY tabStateChanged)")
endforeach()
patch_dependency_source("${ads_headers}" AutoHideTab.h
    "private Q_SLOTS:"
    [=[Q_SIGNALS:
    void tabStateChanged();

private:
    void notifyTabStateChanged();

private Q_SLOTS:]=])
patch_dependency_source("${ads_headers}" AutoHideTab.cpp
    "#include <QTimer>" "#include <QTimer>\n#include <tuple>")
patch_dependency_source("${ads_headers}" AutoHideTab.cpp
    "\tQt::Orientation DragStartOrientation;"
    [=[	Qt::Orientation DragStartOrientation;
    std::tuple<int, Qt::Orientation, bool, bool> LastNotifiedState{
        SideBarLeft, Qt::Vertical, false, false};]=])
patch_dependency_source("${ads_headers}" AutoHideTab.cpp
    [=[		d->updateOrientation();
	}
}]=]
    [=[		d->updateOrientation();
	}
    notifyTabStateChanged();
}]=])
patch_dependency_source("${ads_headers}" AutoHideTab.cpp
    [=[void CAutoHideTab::updateStyle()
{
    internal::repolishStyle]=]
    [=[void CAutoHideTab::notifyTabStateChanged()
{
    const auto state = std::make_tuple(static_cast<int>(sideBarLocation()),
        orientation(), isActiveTab(), iconOnly());
    if (state == d->LastNotifiedState)
        return;
    d->LastNotifiedState = state;
    Q_EMIT tabStateChanged();
}

void CAutoHideTab::updateStyle()
{
    notifyTabStateChanged();
    internal::repolishStyle]=])
patch_dependency_source("${ads_headers}" AutoHideTab.cpp
    "\tsetToolTip(DockWidget->windowTitle());"
    "\tsetToolTip(DockWidget->windowTitle());\n    notifyTabStateChanged();")
# Visibility also changes outside collapseView(), e.g. when a parent hides.
# This class is already a friend of CAutoHideTab; no new public API is needed.
patch_dependency_source("${ads_headers}" AutoHideDockContainer.cpp
    [=[bool CAutoHideDockContainer::event(QEvent* event)
{
	switch]=]
    [=[bool CAutoHideDockContainer::event(QEvent* event)
{
    if (d->SideTab && (event->type() == QEvent::Show || event->type() == QEvent::Hide))
        d->SideTab->notifyTabStateChanged();
	switch]=])
patch_dependency_source("${ads_headers}" DockWidget.cpp
    "\t\td->SideTabWidget->setIcon(Icon);"
    "\t\td->SideTabWidget->setIcon(Icon);\n        d->SideTabWidget->notifyTabStateChanged();")
patch_dependency_source("${ads_headers}" DockManager.h
    "void setDockWidgetFocused(CDockWidget* DockWidget);"
    "void setDockWidgetFocused(ads::CDockWidget* DockWidget);")
# This private geometry query is called directly by friend classes, never as
# a receiving slot. Keep const/private C++ access and meta-object invocability.
patch_dependency_source("${ads_headers}" DockAreaWidget.h
    "\tSideBarLocation calculateSideTabBarArea() const;"
    "private:\n\tQ_INVOKABLE ads::SideBarLocation calculateSideTabBarArea() const;")
