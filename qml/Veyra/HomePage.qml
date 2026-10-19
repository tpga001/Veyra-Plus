// 首页, rebuilt from the design (pages-a.js PAGES.home + pages.css .home).
//
// The design is: a large logo, a two-line greeting, a 4-up grid of source cards
// (164x124 each). Playback history is deliberately absent from this page.
// The logo in the dock returns here; there is no other home affordance.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects

VPage {
    id: root
    signal requestPage(string page)
    readonly property bool localPlayer: typeof veyraDesktop !== "undefined" && veyraDesktop.localPlayer

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 24

        // .home .mark: 128px wide, drop-shadow(0 0 20px rgba(255,255,255,.35)) and the
        // 4.5s "breathe" loop: at 50% the glow is 30px at .55 and the mark scales 1.02.
        // MultiEffect blur is 0..1 of blurMax (32px): 20px = .625, 30px = .94.
        // The layer is padded 40px on every side so the glow has room (a layer on the
        // bare image clips the shadow to the image's own bounds). Negative margins
        // keep the layout the same as a plain 128px image.
        Item {
            id: mark
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: -40
            Layout.bottomMargin: -40
            implicitWidth: logoImg.implicitWidth + 80
            implicitHeight: logoImg.implicitHeight + 80
            property real breath: 0
            scale: 1 + 0.02 * breath
            // Only while the home page shows: running behind other pages it redrew the whole
            // window every vsync during playback (~300 presents/s on a 320 Hz screen), which
            // cost GPU time and kept OBS game capture on the UI instead of the video.
            SequentialAnimation on breath {
                running: !Theme.reduced && mark.visible
                loops: Animation.Infinite
                NumberAnimation { to: 1; duration: Theme.d(2250); easing.type: Easing.InOutSine }
                NumberAnimation { to: 0; duration: Theme.d(2250); easing.type: Easing.InOutSine }
            }
            Image {
                id: logoImg
                anchors.centerIn: parent
                source: "logo.png"
                sourceSize.width: 128
                fillMode: Image.PreserveAspectFit
            }
            // A sibling MultiEffect, not layer.effect: Qt recreates a layer's effect item on
            // a screen DPI change while it walks the parent's children, and the walk then touched
            // the deleted item (crash moving the window to a 200 % monitor, field 2026-10-01).
            MultiEffect {
                source: logoImg
                anchors.fill: logoImg
                shadowEnabled: true
                shadowColor: "#FFFFFF"
                shadowHorizontalOffset: 0
                shadowVerticalOffset: 0
                blurMax: 64
                shadowBlur: 0.625 + 0.3125 * mark.breath
                shadowOpacity: 0.35 + 0.2 * mark.breath
            }
        }

        // .hello: .h1 plus a muted line.
        ColumnLayout {
            id: hello
            Layout.alignment: Qt.AlignHCenter
            spacing: 6
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("今天看点什么？")
                color: Theme.t1
                font.family: Theme.fontUi
                font.pixelSize: Theme.fsH1
                font.weight: Font.DemiBold; font.variableAxes: Theme.axesDemiBold
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("选一个片源开始，画质预设和补帧随时在播放栏切换")
                color: Theme.t2
                font.family: Theme.fontUi
                font.pixelSize: Theme.fsBody
            }
        }

        // Seven equal source cards. A narrow window wraps the same entries onto a second row.
        Flow {
            id: srcGrid
            objectName: "home-source-grid"
            Layout.alignment: Qt.AlignHCenter
            readonly property int sourceColumns: Math.max(1, Math.min(7, Math.floor((root.width - 36) / 176)))
            Layout.preferredWidth: sourceColumns * 176 - 12
            spacing: 12
            // Subtitles name what is really there: the device of the last capture
            // session and the saved PS5 host; generic wording when there is none.
            // Only the subtitle follows the live state. The cards themselves are a fixed
            // list: when the whole array was one binding, every Xbox / PC state change
            // (polling after sign-in) destroyed and rebuilt all six cards, and a click
            // landing during the rebuild was lost (field report 2026-10-02).
            function cardSub(act) {
                switch (act) {
                case "file": return qsTr("MP4 · MKV · 图片")
                case "browser": return root.localPlayer ? qsTr("在常驻进程中运行") : veyra.browserEnabled ? qsTr("已开启 · ") + (veyra.browserTelemetry.live ? qsTr("播放中") : veyra.browserTelemetry.hasLastSample ? qsTr("已冻结") : qsTr("等待视频")) : qsTr("Chrome · Edge 视频")
                case "capture": return veyra.hasCaptureSession ? veyra.captureSessionSummary.split(" · ")[0] : qsTr("HDMI 采集设备")
                case "ps5": return veyra.remotePlayHost.length > 0 ? qsTr("已保存主机 ") + veyra.remotePlayHost : qsTr("局域网串流")
                case "moonlight": return veyra.moonlight && veyra.moonlight.state.lastLabel ? veyra.moonlight.state.lastLabel : qsTr("Sunshine 主机")
                case "xbox": return veyra.xbox && veyra.xbox.state.lastLabel ? veyra.xbox.state.lastLabel : qsTr("账号登录 · 实验")
                default: return qsTr("窗口或显示器")
                }
            }
            Repeater {
                model: [
                    { glyph: "folder", title: qsTr("打开视频"), act: "file" },
                    { glyph: "globe", title: qsTr("浏览器增强"), act: "browser" },
                    { glyph: "video", title: qsTr("采集卡"), act: "capture" },
                    { glyph: "gamepad", title: qsTr("PS5 串流"), act: "ps5" },
                    { glyph: "cast", title: qsTr("PC 串流"), act: "moonlight" },
                    { glyph: "gamepad", title: qsTr("Xbox 串流"), act: "xbox" },
                    { glyph: "monitor", title: qsTr("屏幕捕获"), act: "screen" }
                ]
                // The layout owns the slot's position, so the card inside it is free to
                // move its own y for the hover lift (a y set on a layout child is
                // overwritten); the entrance rise goes on the slot.
                delegate: Item {
                    id: slot
                    objectName: "home-source-" + modelData.act
                    required property var modelData
                    required property int index
                    implicitWidth: 164
                    implicitHeight: 124
                    VRise { page: root; target: slot; d: 2 + slot.index }
                  Rectangle {
                    id: srcCard
                    readonly property var modelData: slot.modelData
                    // .srccard: 164x124, radius 14, content pinned top and bottom.
                    width: 164
                    height: 124
                    radius: Theme.rCard
                    color: cardHover.hovered ? Theme.card2 : Theme.card
                    border.width: 1
                    border.color: cardHover.hovered ? Theme.stroke2 : Theme.stroke
                    // .srccard:hover { transform: translateY(-4px) }
                    y: cardHover.hovered ? -4 : 0
                    Behavior on y { NumberAnimation { duration: Theme.d(500); easing.bezierCurve: Theme.spring } }
                    scale: cardTap.pressed ? 0.97 : 1.0
                    Behavior on scale { NumberAnimation { duration: Theme.d(500); easing.bezierCurve: Theme.spring } }

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 0
                        Rectangle {
                            // .srccard .ico: 34x34, radius 10, translucent plate.
                            implicitWidth: 34
                            implicitHeight: 34
                            radius: 10
                            color: Qt.rgba(1, 1, 1, 0.06)
                            VIcon {
                                anchors.centerIn: parent
                                name: modelData.glyph
                            }
                        }
                        Item { Layout.fillHeight: true }
                        ColumnLayout {
                            spacing: 3
                            Text {
                                text: modelData.title
                                color: Theme.t1
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fsBody
                                font.weight: Font.Medium
                            }
                            Text {
                                text: srcGrid.cardSub(modelData.act)
                                color: Theme.t3
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fsSmall
                            }
                        }
                    }
                    HoverHandler { id: cardHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        id: cardTap
                        objectName: "home-source-action-" + modelData.act
                        onTapped: {
                            veyra.logUi("ui-source", "home card act=" + modelData.act)
                            switch (modelData.act) {
                            case "file": veyra.openFileDialog(); break
                            case "browser":
                                if (root.localPlayer) veyraDesktop.showResidentWindow()
                                else veyra.openBrowserDialog()
                                break
                            case "capture": veyra.openCaptureDialog(); break
                            case "ps5": veyra.openPs5Dialog(); break
                            case "moonlight": veyra.openMoonlightDialog(); break
                            case "xbox": veyra.openXboxDialog(); break
                            case "screen": veyra.openScreenCaptureDialog(); break
                            }
                        }
                    }
                  }
                }
            }
        }

    }

    // [data-in] entrance order from pages-a.js (the source cards rise from their delegate, --d 2-5).
    VRise { target: mark; d: 0 }
    VRise { target: hello; d: 1 }
}
