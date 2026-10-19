// Minimal mode reserves a full-width transport row below the native video.
// Main.qml owns the viewport and the independently composited control window.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

VPage {
    id: root
    signal requestPage(string page)

    // cineIn (pages.css @keyframes cineIn, plan M21 "上下各 12% 裁切展开 | 子窗口区域"):
    // the stage opens from inset(12% 0 12% 0 round 8px) to inset(0). The picture is a
    // native window above this scene, so a QML clip would not touch it - main.cpp
    // reads this item and insets the video window's REGION instead (no resize, so no
    // swapchain churn). It carries the value only: invisible, 0x0, never drawn. The
    // C++ walk does not filter on visibility (unlike the cover walk, which must), so
    // an invisible carrier still reaches it.
    Item {
        id: cineIn
        objectName: "videoInset"
        property real frac: 0
        visible: false
        width: 0
        height: 0
    }
    // The animation itself: 12% -> 0 over .8s --spring-soft, started when the page
    // shows through an animated switch (VPage.enter). A cold start (first open) has
    // nothing to animate from, so it goes straight to 0.
    NumberAnimation {
        id: cineInAnim
        target: cineIn
        property: "frac"
        to: 0
        duration: Theme.d(800)
        easing.bezierCurve: Theme.springSoft
    }
    onEnter: { cineIn.frac = 0.12; cineInAnim.restart() }

    // Keep the native picture above the reserved playback controls.
    readonly property real pictureHeight: parent ? Math.max(0, parent.height - (parent.width < 640 ? 78 : 48)) : 0

    // --- the picture ------------------------------------------------------
    // .min .stage: full width, the picture height, radius 8 (the window radius),
    // pure black behind. The native video window is placed here by main.cpp; the
    // radius applies to the black plate, never to the video, because rounding the
    // picture crops it.
    Rectangle {
        id: stage
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: root.pictureHeight
        radius: Theme.rWindow
        color: Theme.videoBlack
        clip: true

        Text {
            anchors.centerIn: parent
            visible: veyra.sourceKind === "browser" || stageTap.enabled
            width: Math.max(0, parent.width - 40)
            horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
            text: veyra.sourceKind === "browser" ? qsTr("画面在浏览器视频区域显示") + "\n" + veyra.browserStatus : qsTr("点击画面选择片源")
            color: Theme.t3
            font.family: Theme.fontUi
            font.pixelSize: Theme.fsH3
        }
        // Nothing open: a click on the empty picture opens the source menu where it
        // was clicked (the same menu as the professional page's 片源 button).
        Item { id: menuAnchor; width: 1; height: 1 }
        TapHandler {
            id: stageTap
            enabled: !veyra.hasSource && veyra.liveOpeningText.length === 0
            onTapped: eventPoint => {
                menuAnchor.x = eventPoint.position.x
                menuAnchor.y = eventPoint.position.y
                sourceMenu.openAt(menuAnchor, "at")
            }
        }
        // A PS5 / capture session that is still connecting: a pill in the
        // middle of the (still black) picture, cut out of the native video.
        Rectangle {
            objectName: "min-live-opening"
            readonly property bool videoCover: true
            property real coverRadius: height / 2
            visible: veyra.liveOpeningText.length > 0
            anchors.centerIn: parent
            width: openingRow.implicitWidth + 32
            height: 40
            radius: height / 2
            color: Qt.rgba(22 / 255, 22 / 255, 26 / 255, 0.92)
            border.width: 1
            border.color: Theme.stroke2
            Row {
                id: openingRow
                anchors.centerIn: parent
                spacing: 10
                VSpinner { anchors.verticalCenter: parent.verticalCenter }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: veyra.liveOpeningText
                    color: Theme.t1
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fsBody
                }
            }
        }
    }

    VMenu {
        id: sourceMenu
        objectName: "min-source-menu"
        title: qsTr("片源")
        items: [{ label: qsTr("打开文件…"), icon: "folder", act: "file" },
                     { label: qsTr("浏览器增强…"), icon: "globe", act: "browser" },
                { label: qsTr("PS5 串流…"), icon: "gamepad", act: "ps5" },
                { label: qsTr("PC 串流…"), icon: "cast", act: "moonlight" },
                { label: qsTr("Xbox 串流…"), icon: "gamepad", act: "xbox" },
                { label: qsTr("屏幕捕获…"), icon: "monitor", act: "screen" },
                { sep: true },
                { label: qsTr("采集卡设置…"), icon: "settings", act: "capture" }]
        onPicked: (i, o) => {
            if (o.act === "file") veyra.openFileDialog()
            else if (o.act === "browser") veyra.openBrowserDialog()
            else if (o.act === "ps5") veyra.openPs5Dialog()
            else if (o.act === "moonlight") veyra.openMoonlightDialog()
            else if (o.act === "xbox") veyra.openXboxDialog()
            else if (o.act === "screen") veyra.openScreenCaptureDialog()
            else if (o.act === "capture") veyra.openCaptureDialog()
        }
    }
    readonly property bool sourceMenuOpen: sourceMenu.visible
    function closeSourceMenu() { sourceMenu.close() }
    function openSourceMenuForTest() { menuAnchor.x = width / 2; menuAnchor.y = pictureHeight / 2; sourceMenu.openAt(menuAnchor, "at") }

    // Report the film aspect upward so the window can snap to it.
    onPictureHeightChanged: if (veyra.sourceAspect > 0.2) root.requestAspect(veyra.sourceAspect)
    signal requestAspect(real aspect)
    signal requestFullscreen()
}
