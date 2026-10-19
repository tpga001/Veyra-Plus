// G2.5: the fullscreen control bar, in its own top-level window.
//
// In fullscreen the picture is the whole screen, and the native D3D12 video window
// is drawn above everything in the main window's scene. A bar in the main scene
// would need a hole cut out of the picture; the design (and D1 in the goal plan)
// puts it in a separate small window instead. It is owned by the main window
// (always above it, never above other applications), takes no keyboard focus (the
// shortcuts stay with the main window), and is hidden outright when the controls
// hide, so a hidden bar composes nothing over the video.
//
// The window is taller than the pill so the preset menu can open upwards inside it;
// the pointer only hits the pill (or the open menu) because the rest is masked off.
import QtQuick
import QtQuick.Window

Window {
    id: win
    objectName: "fullscreenBar"
    required property Window owner
    property bool shown: false
    signal requestPage(string page)
    signal requestDialog(string key)
    signal requestFullscreen()
    signal requestLock()
    signal requestMove()
    signal requestHide()
    // Pointer movement over the bar keeps the controls up (the main window cannot
    // see it here).
    signal activity()
    readonly property var activeBar: compact ? minimalBar : bar
    readonly property bool hovered: activeBar.hovered
    readonly property bool menuOpen: activeBar.menuOpen
    // G3.2 (D1): the same window carries the cinema pill in windowed 极简 mode, where
    // it straddles the picture's bottom edge: its top is pillTop (owner-relative),
    // half over the native video window and half below it. In fullscreen it sits
    // 24px above the screen edge.
    property bool cinema: false
    readonly property bool compact: owner.cinema
    readonly property int barHeight: compact ? owner.minControlBarHeight : 92
    property real pillTop: 0

    transientParent: owner
    // Follow the owner explicitly; coordinates and the native mask must share
    // the same logical screen when playback moves between DPI-scaled monitors.
    screen: owner.screen
    flags: Qt.Tool | Qt.FramelessWindowHint | Qt.WindowDoesNotAcceptFocus
    color: "transparent"

    readonly property rect screenBounds: owner.screen
        ? Qt.rect(owner.screen.virtualX, owner.screen.virtualY, owner.screen.width, owner.screen.height)
        : Qt.rect(owner.x, owner.y, owner.width, owner.height)
    // High DPI can leave only 540 logical pixels on a 1080p screen. Keep both
    // the complete pill and its upward menus inside that screen.
    readonly property int menuRoom: Math.max(72, Math.min(440, Math.floor(compact
        ? owner.y + owner.height - barHeight - screenBounds.y - 16
        : screenBounds.height - barHeight - 24 - 16)))
    // Room for all transport controls and long preset names. A portrait cinema
    // window still lets the pill reach past the picture, but never past its screen.
    readonly property int barWidth: compact ? Math.max(1, owner.width) : Math.max(1, Math.min(1000, Math.max(1, screenBounds.width - 48),
                                                       Math.max(760, owner.width - 48)))
    width: barWidth
    height: barHeight + menuRoom
    // Centred; fullscreen: 24px above the screen's bottom edge (the design's 24px
    // side margin); cinema: .cine-bar top = --pic-h - 46. Kept on the owner's screen.
    x: {
        if (compact) return owner.x
        const cx = owner.x + Math.round((owner.width - width) / 2)
        return Math.max(screenBounds.x + 24, Math.min(cx, screenBounds.x + screenBounds.width - width - 24))
    }
    y: compact ? owner.y + owner.height - height
               : cinema ? owner.y + Math.round(pillTop) - menuRoom
              : screenBounds.y + screenBounds.height - height - 24

    // Hidden outright rather than transparent, so it composes nothing over the video
    // (the PresentMon comparison in the plan measures exactly this). The fade is
    // the design's control fade: opacity .2s; the window goes when it finishes.
    property real fade: shown ? 1 : 0
    Behavior on fade { NumberAnimation { duration: Theme.d(200) } }
    visible: owner.visible && owner.visibility !== Window.Minimized && (shown || fade > 0.01)
    function closeMenus() { if (minimalBar) minimalBar.closeMenus(); if (bar) bar.closeMenus() }
    onCompactChanged: closeMenus()
    onVisibleChanged: {
        veyra.logUi("ui-fullscreen", "control window visible=" + visible)
        if (!visible) closeMenus()
    }

    // The seek preview rises above the pill, so its window region must include it.
    // With the menu open, the whole window takes input to allow outside-click close.
    // The pill's hover tips open above its buttons; while the pointer is on the pill
    // a band above it stays in the region, or the mask would cut the tip off and the
    // picture would show through it (field report 2026-10-02).
    readonly property int tipRoom: 56
    readonly property rect hitRect: menuOpen ? Qt.rect(0, 0, width, height)
                                             : activeBar.seekPreviewOpen ? Qt.rect(0, Math.max(0, menuRoom - 122), width, Math.min(height, barHeight + 122))
                                             : activeBar.hovered ? Qt.rect(0, menuRoom - tipRoom, width, barHeight + tipRoom)
                                             : Qt.rect(0, menuRoom, width, barHeight)
    onHitRectChanged: veyra.setWindowMask(win, hitRect)
    Component.onCompleted: veyra.setWindowMask(win, hitRect)

    // barIn (pages.css): from opacity 0, translate 30px, scale .94, .8s --spring
    // after .12s. Played by enter() when the cinema page shows.
    property real enterDy: 0
    property real enterScale: 1
    property real enterOpacity: 1
    function enter() { barIn.restart() }
    // The cinema pill enters with barIn each time it appears (page shown, dialog
    // closed); the fullscreen one keeps the plain fade and rise.
    onShownChanged: {
        if (!shown) closeMenus()
        else if (cinema && !compact) enter()
    }
    SequentialAnimation {
        id: barIn
        ScriptAction { script: { win.enterDy = 30; win.enterScale = 0.94; win.enterOpacity = 0 } }
        PauseAnimation { duration: Theme.d(120) }
        ParallelAnimation {
            NumberAnimation { target: win; property: "enterDy"; to: 0; duration: Theme.d(800); easing.bezierCurve: Theme.spring }
            NumberAnimation { target: win; property: "enterScale"; to: 1; duration: Theme.d(800); easing.bezierCurve: Theme.spring }
            NumberAnimation { target: win; property: "enterOpacity"; to: 1; duration: Theme.d(800); easing.bezierCurve: Theme.spring }
        }
    }

    CineBar {
        id: bar
        visible: !win.compact
        x: 0
        y: win.menuRoom
        width: win.width
        height: 92
        fullscreen: !win.cinema
        menuBottomLimit: win.menuRoom - 16
        opacity: win.fade * win.enterOpacity
        // barIn-like rise while showing: translate 30px -> 0 with the fade.
        transform: Translate { y: (win.cinema ? 0 : (1 - win.fade) * 30) + win.enterDy }
        scale: win.enterScale
        onRequestPage: p => win.requestPage(p)
        onRequestDialog: k => win.requestDialog(k)
        onRequestFullscreen: win.requestFullscreen()
        onRequestLock: win.requestLock()
        onRequestMove: win.requestMove()
        onRequestHide: win.requestHide()
        HoverHandler {
            // Real movement only: hover repeats while the bar animates.
            property point last: Qt.point(-1, -1)
            onPointChanged: {
                const p = point.scenePosition
                if (Math.abs(p.x - last.x) < 1 && Math.abs(p.y - last.y) < 1) return
                last = p
                win.activity()
            }
        }
    }
    CompactPlaybackBar {
        id: minimalBar
        visible: win.compact
        x: 0; y: win.menuRoom; width: win.width; height: win.barHeight
        menuBottomLimit: win.menuRoom - 16
        opacity: win.fade
        onRequestDialog: key => win.requestDialog(key)
        onRequestFullscreen: win.requestFullscreen()
    }
}
