# Pinned ADS preview delegates already accept explicit global positions on
# Wayland. Use the delivered Qt mouse position on the other platforms too;
# this also makes app-owned isolated input follow the real production path.
function(workspace_patch_ads filename marker anchor replacement)
    set(path "${ADS_SOURCE}/src/${filename}")
    file(READ "${path}" content)
    string(REPLACE "\r\n" "\n" content "${content}")
    if(NOT content MATCHES "${marker}")
        string(FIND "${content}" "${anchor}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "ADS ${filename} ${marker} no longer matches the pinned source")
        endif()
        string(REPLACE "${anchor}" "${replacement}" content "${content}")
        file(WRITE "${path}" "${content}")
    endif()
endfunction()

set(anchor [=[
	// Only stash the event-supplied position on Wayland, where QCursor::pos()
	// is unreliable and finishDragging() must reuse it. On other platforms
	// finishDragging() keeps using the live QCursor::pos() (unchanged behavior).
	if (internal::isWayland())
	{
		d->LastGlobalPos = GlobalPos;
		d->HasLastGlobalPos = true;
	}
]=])
set(replacement [=[
    // Application event position authoritative for preview drop.
    d->LastGlobalPos = GlobalPos;
    d->HasLastGlobalPos = true;
]=])
workspace_patch_ads(FloatingDragPreview.cpp "Application event position authoritative" "${anchor}" "${replacement}")

foreach(filename IN ITEMS DockWidgetTab.cpp DockAreaTitleBar.cpp)
    set(anchor "            d->FloatingWidget->moveFloating();")
    set(replacement [=[
            // Application mouse event drives the library preview.
            if (auto *preview = dynamic_cast<CFloatingDragPreview*>(d->FloatingWidget))
                preview->moveFloating(internal::globalPositionOf(ev));
            else
                d->FloatingWidget->moveFloating();
]=])
    workspace_patch_ads("${filename}" "Application mouse event drives" "${anchor}" "${replacement}")
    set(anchor "d->FloatingWidget->finishDragging();")
    set(replacement [=[
// Application release position completes the same preview delegate.
            if (auto *preview = dynamic_cast<CFloatingDragPreview*>(d->FloatingWidget))
                preview->moveFloating(internal::globalPositionOf(ev));
            d->FloatingWidget->finishDragging();]=])
    workspace_patch_ads("${filename}" "Application release position completes" "${anchor}" "${replacement}")
endforeach()

# Reordering moves the tab before checking whether the pointer left its bar.
# Event-local coordinates refer to the tab's position at event delivery; after
# moveTab(), mapping them through the moved tab applies the movement twice.
set(anchor "    auto MappedPos = mapToParent(ev->pos());")
set(replacement "    // Application reorder hit test uses the stable event global position.\n    auto MappedPos = parentWidget()->mapFromGlobal(internal::globalPositionOf(ev));")
workspace_patch_ads(DockWidgetTab.cpp "Application reorder hit test" "${anchor}" "${replacement}")

# Windows floating areas keep an application tab/header (placement controls).
# Its Qt drag must be able to re-dock, just like dragging an individual tab.
# Keep the compositor-owned Wayland branch unchanged.
set(anchor [=[
	if (d->DockArea->dockContainer()->isFloating()
	 && d->DockArea->dockContainer()->visibleDockAreaCount() == 1
     && !d->DockArea->isAutoHide())]=])
set(replacement [=[
    // Application floating header can enter a preview on non-Wayland.
	if (internal::isWayland() && d->DockArea->dockContainer()->isFloating()
	 && d->DockArea->dockContainer()->visibleDockAreaCount() == 1
     && !d->DockArea->isAutoHide())]=])
workspace_patch_ads(DockAreaTitleBar.cpp "Application floating header can enter" "${anchor}" "${replacement}")

# A single-area target still accepts tab insertion. Multi-area/container edge
# previews retain their old priority everywhere outside the real header hit.
foreach(filename IN ITEMS FloatingDragPreview.cpp DockContainerWidget.cpp)
    set(anchor "(VisibleDockAreas == 1) ? NoDockWidgetArea : DockArea->allowedAreas()")
    set(replacement "/* Application single-area tab insertion */ (VisibleDockAreas == 1) ? CenterDockWidgetArea : DockArea->allowedAreas()")
    workspace_patch_ads("${filename}" "Application single-area tab insertion" "${anchor}" "${replacement}")
endforeach()
set(anchor "if ((Area == CenterDockWidgetArea) && (ContainerDropArea != InvalidDockWidgetArea))")
set(replacement "// Application tabbar wins over container edge preview.\n\t\tif ((Area == CenterDockWidgetArea) && (ContainerDropArea != InvalidDockWidgetArea) &&\n            DockAreaOverlay->tabIndexUnderCursor() < TabDefaultInsertIndex)")
workspace_patch_ads(FloatingDragPreview.cpp "Application tabbar wins" "${anchor}" "${replacement}")
set(anchor "if ((Area == CenterDockWidgetArea)\n\t\t    && (ContainerArea != InvalidDockWidgetArea))")
set(replacement "// Application tabbar wins over native container edge preview.\n\t\tif ((Area == CenterDockWidgetArea)\n\t\t    && (ContainerArea != InvalidDockWidgetArea) &&\n            DockAreaOverlay->tabIndexUnderCursor() < TabDefaultInsertIndex)")
workspace_patch_ads(DockContainerWidget.cpp "Application tabbar wins over native" "${anchor}" "${replacement}")
set(anchor "if (ContainerDropArea != InvalidDockWidgetArea &&\n\t\t\tContainerDropArea != dropArea)")
set(replacement "// Application native tabbar drop retains the exact insertion index.\n\t\tif (ContainerDropArea != InvalidDockWidgetArea &&\n\t\t\tContainerDropArea != dropArea && dropOverlay->tabIndexUnderCursor() < TabDefaultInsertIndex)")
workspace_patch_ads(DockContainerWidget.cpp "Application native tabbar drop retains" "${anchor}" "${replacement}")
