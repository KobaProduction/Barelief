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
# Dock tab icon size is mutable, and assignment may refresh the existing icon.
patch_dependency_source("${ads_headers}" DockWidgetTab.h
    "Q_PROPERTY(QSize iconSize READ iconSize WRITE setIconSize)"
    "Q_PROPERTY(QSize iconSize READ iconSize WRITE setIconSize NOTIFY iconSizeChanged)")
patch_dependency_source("${ads_headers}" DockWidgetTab.h
    "\tvoid activeTabChanged();"
    "\tvoid activeTabChanged();\n    void iconSizeChanged();")
patch_dependency_source("${ads_headers}" DockWidgetTab.cpp
    [=[	d->IconSize = Size;
	d->updateIcon();
}]=]
    [=[    const bool changed = d->IconSize != Size;
	d->IconSize = Size;
	d->updateIcon();
    if (changed)
        Q_EMIT iconSizeChanged();
}]=])
# Keep the upstream styling setters and dummy C++ getters, while the Qt
# properties now read the configured state they notify about.
patch_dependency_source("${ads_headers}" DockOverlay.h
    "Q_PROPERTY(QString iconColors READ iconColors WRITE setIconColors)"
    "Q_PROPERTY(QString iconColors READ configuredIconColors WRITE setIconColors NOTIFY iconColorsChanged)")
foreach(component IN ITEMS Frame Background Overlay Arrow Shadow)
    patch_dependency_source("${ads_headers}" DockOverlay.h
        "Q_PROPERTY(QColor icon${component}Color READ iconColor WRITE setIcon${component}Color)"
        "Q_PROPERTY(QColor icon${component}Color READ configured${component}Color WRITE setIcon${component}Color NOTIFY iconColorsChanged)")
endforeach()
patch_dependency_source("${ads_headers}" DockOverlay.h
    [=[Q_PROPERTY(QColor iconShadowColor READ configuredShadowColor WRITE setIconShadowColor NOTIFY iconColorsChanged)

public:]=]
    [=[Q_PROPERTY(QColor iconShadowColor READ configuredShadowColor WRITE setIconShadowColor NOTIFY iconColorsChanged)

Q_SIGNALS:
    void iconColorsChanged();

public:]=])
patch_dependency_source("${ads_headers}" DockOverlay.h
    "    QString iconColors() const;"
    [=[    QString iconColors() const;
    QString configuredIconColors() const;
    QColor configuredFrameColor() const {return iconColor(FrameColor);}
    QColor configuredBackgroundColor() const {return iconColor(WindowBackgroundColor);}
    QColor configuredOverlayColor() const {return iconColor(OverlayColor);}
    QColor configuredArrowColor() const {return iconColor(ArrowColor);}
    QColor configuredShadowColor() const {return iconColor(ShadowColor);}]=])
patch_dependency_source("${ads_headers}" DockOverlay.cpp
    "#include <QMap>" "#include <QMap>\n#include <QStringList>")
patch_dependency_source("${ads_headers}" DockOverlay.cpp
    [=[	d->IconColors[ColorIndex] = Color;
	d->UpdateRequired = true;]=]
    [=[    const bool Changed = d->IconColors[ColorIndex] != Color;
	d->IconColors[ColorIndex] = Color;
	d->UpdateRequired = true;
    if (Changed)
        Q_EMIT iconColorsChanged();]=])
patch_dependency_source("${ads_headers}" DockOverlay.cpp
    "    auto ColorList = Colors.split(' ', SkipEmptyParts);"
    "    bool Changed = false;\n    auto ColorList = Colors.split(' ', SkipEmptyParts);")
patch_dependency_source("${ads_headers}" DockOverlay.cpp
    [=[		d->IconColors[Component] = QColor(ComponentColor[1]);
	}

	d->UpdateRequired = true;]=]
    [=[        const QColor Color(ComponentColor[1]);
        Changed |= d->IconColors[Component] != Color;
		d->IconColors[Component] = Color;
	}

	d->UpdateRequired = true;
    if (Changed)
        Q_EMIT iconColorsChanged();]=])
patch_dependency_source("${ads_headers}" DockOverlay.cpp
    [=[QString CDockOverlayCross::iconColors() const
{
	return QString();
}]=]
    [=[QString CDockOverlayCross::iconColors() const
{
	return QString();
}

QString CDockOverlayCross::configuredIconColors() const
{
    static const char* Names[] = {"Frame", "Background", "Overlay", "Arrow", "Shadow"};
    QStringList Colors;
    for (int Index = 0; Index < 5; ++Index)
    {
        const auto Color = iconColor(static_cast<eIconColor>(Index));
        if (Color.isValid())
            Colors.append(QString::fromLatin1(Names[Index]) + QLatin1Char('=') + Color.name(QColor::HexArgb));
    }
    return Colors.join(QLatin1Char(' '));
}]=])
patch_dependency_source("${ads_headers}" AutoHideDockContainer.h
    [=[Q_PROPERTY(int sideBarLocation READ sideBarLocation)
private:]=]
    [=[Q_PROPERTY(int sideBarLocation READ sideBarLocation NOTIFY sideBarLocationChanged)
Q_SIGNALS:
    void sideBarLocationChanged();
private:]=])
patch_dependency_source("${ads_headers}" AutoHideDockContainer.cpp
    [=[	d->ResizeHandle->setHandlePosition(edgeFromSideTabBarArea(SideBarLocation));
    internal::repolishStyle(this, internal::RepolishDirectChildren);]=]
    [=[	d->ResizeHandle->setHandlePosition(edgeFromSideTabBarArea(SideBarLocation));
    internal::repolishStyle(this, internal::RepolishDirectChildren);
    Q_EMIT sideBarLocationChanged();]=])

# Tab hit-testing uses scroll-container coordinates and only visible tabs.
set(ads_tab_hit_testing [=[int CDockAreaTabBar::tabAt(const QPoint& Pos) const
{
	if (!isVisible())
	{
		return TabInvalidIndex;
	}
	const auto ContainerPos = d->TabsContainerWidget->mapFrom(this, Pos);
	bool FirstVisibleTab = true;
	for (int i = 0; i < count(); ++i)
	{
		const auto* CurrentTab = tab(i);
		if (!CurrentTab->isVisibleTo(this))
		{
			continue;
		}
		const auto Geometry = CurrentTab->geometry();
		if (FirstVisibleTab && ContainerPos.x() < Geometry.x())
		{
			return -1;
		}
		FirstVisibleTab = false;
		if (Geometry.contains(ContainerPos))
		{
			return i;
		}
	}
	return count();
}]=])
file(READ "${ads_headers}/DockAreaTabBar.cpp" ads_tab_bar_source)
string(REPLACE "\r\n" "\n" ads_tab_bar_source "${ads_tab_bar_source}")
string(FIND "${ads_tab_bar_source}" "${ads_tab_hit_testing}" ads_tab_hit_testing_present)
if(ads_tab_hit_testing_present EQUAL -1)
# Tab hit-testing receives bar coordinates; tab geometries belong to the scroll container.
patch_dependency_source("${ads_headers}" DockAreaTabBar.cpp
    [=[int CDockAreaTabBar::tabAt(const QPoint& Pos) const
{
	if (!isVisible())
	{
		return TabInvalidIndex;
	}

	if (Pos.x() < tab(0)->geometry().x())
	{
		return -1;
	}

	for (int i = 0; i < count(); ++i)
	{
		if (tab(i)->geometry().contains(Pos))
		{
			return i;
		}
	}

	return count();
}]=]
    [=[int CDockAreaTabBar::tabAt(const QPoint& Pos) const
{
	if (!isVisible())
	{
		return TabInvalidIndex;
	}

	const auto ContainerPos = d->TabsContainerWidget->mapFrom(this, Pos);
	if (ContainerPos.x() < tab(0)->geometry().x())
	{
		return -1;
	}

	for (int i = 0; i < count(); ++i)
	{
		if (tab(i)->geometry().contains(ContainerPos))
		{
			return i;
		}
	}

	return count();
}]=])
patch_dependency_source("${ads_headers}" DockAreaTabBar.cpp
    [=[int CDockAreaTabBar::tabAt(const QPoint& Pos) const
{
	if (!isVisible())
	{
		return TabInvalidIndex;
	}

	const auto ContainerPos = d->TabsContainerWidget->mapFrom(this, Pos);
	if (ContainerPos.x() < tab(0)->geometry().x())
	{
		return -1;
	}

	for (int i = 0; i < count(); ++i)
	{
		if (tab(i)->geometry().contains(ContainerPos))
		{
			return i;
		}
	}

	return count();
}]=]
    "${ads_tab_hit_testing}")
endif()
unset(ads_tab_hit_testing)
unset(ads_tab_bar_source)
unset(ads_tab_hit_testing_present)

# insertDockWidget blocks tabInserted/currentChanged during layout insertion.
# An already visible moved tab emits no Show event, so invalidate the cached
# title-bar menu explicitly after the final visibility has been installed.
patch_dependency_source("${ads_headers}" DockAreaWidget.cpp
    "\tTabWidget->setVisible(!DockWidget->isClosed());"
    "\tTabWidget->setVisible(!DockWidget->isClosed());\n\td->TitleBar->markTabsMenuOutdated();")

# Reparenting or closing a previously active tab can leave an inactive open
# tab hidden. Restore its logical visibility without activating the page;
# CDockWidgetTab::setVisible retains the widget's NoTab feature semantics.
patch_dependency_source("${ads_headers}" DockAreaTabBar.cpp
    "\t\telse\n\t\t{\n\t\t\tTabWidget->setActiveTab(false);"
    "\t\telse\n\t\t{\n\t\t\tTabWidget->setVisible(!TabWidget->dockWidget()->isClosed());\n\t\t\tTabWidget->setActiveTab(false);")
