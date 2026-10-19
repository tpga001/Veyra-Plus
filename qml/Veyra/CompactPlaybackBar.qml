// Minimal playback controls dock below the picture, matching the professional transport row.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: bar
    objectName: "compactPlaybackBar"
    readonly property bool browserSource: veyra.sourceKind === "browser"
    readonly property bool compact: width < 640
    property real menuBottomLimit: -1
    signal requestDialog(string key)
    signal requestFullscreen()
    readonly property bool hovered: barHover.hovered
    readonly property bool seekPreviewOpen: seekMouse.containsMouse && seekMouse.enabled
    readonly property bool menuOpen: ccMenu.visible || audioMenu.visible || playbackRate.menuOpen
    function closeMenus() {
        if (ccMenu) ccMenu.close()
        if (audioMenu) audioMenu.close()
        if (playbackRate) playbackRate.closeMenus()
    }
    onVisibleChanged: if (!visible) closeMenus()
    implicitHeight: compact ? 78 : 48
    radius: 8
    color: "#16161A"
    HoverHandler { id: barHover }

    component BarButton: Item {
        id: button
        property string glyph: ""
        property string tip: ""
        signal tapped()
        implicitWidth: 28; implicitHeight: 28
        VIcon { anchors.centerIn: parent; name: button.glyph; color: hover.hovered ? Theme.t1 : Theme.t2 }
        HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
        TapHandler { gesturePolicy: TapHandler.WithinBounds; onTapped: button.tapped() }
        ToolTip.visible: hover.hovered && !bar.menuOpen
        ToolTip.text: button.tip
    }
    RowLayout {
        objectName: "minimal-transport"
        visible: !bar.browserSource
        anchors.left: parent.left; anchors.right: parent.right
        anchors.leftMargin: 12; anchors.rightMargin: bar.compact ? 12 : actions.width + 24
        y: 8; height: 32; spacing: 10
        BarButton {
            objectName: "minimal-play-pause"
            glyph: veyra.running && !veyra.paused ? "pause" : "play"
            tip: veyra.paused ? qsTr("播放") : qsTr("暂停")
            onTapped: veyra.togglePlayPause()
        }
        Text {
            objectName: "minimal-position"
            text: veyra.positionText; color: Theme.t2
            font.family: Theme.fontMono; font.pixelSize: Theme.fsSmall
        }
                Item {
                    id: seekArea
                    objectName: "minimal-seek"
                    Layout.fillWidth: true
                    implicitHeight: 14
                    property bool scrubbing: false
                    property real scrubFrac: 0
                    // .seek:hover .thumb { left: 44% } - the thumb tracks the real
                    // progress, not the design's hard-coded 44%.
                    readonly property real frac: scrubbing ? scrubFrac : Math.max(0, Math.min(1, shownProgress))
                    // Twice a second, not every snapshot: the pill is its own window and a rail
                    // that moved every tick made it present ~50 times a second, more often than
                    // a film plays, so OBS game capture could lock onto the pill.
                    property real shownProgress: veyra.progress
                    Timer {
                        interval: 500; repeat: true; running: bar.visible
                        onTriggered: seekArea.shownProgress = veyra.progress
                    }
                    readonly property real hoverFrac: scrubbing ? scrubFrac
                                                      : seekMouse.containsMouse && seekArea.width > 0
                                                        ? Math.max(0, Math.min(1, seekMouse.mouseX / seekArea.width))
                                                        : frac
                    property int thumbnailBucket: -1
                    function fractionAt(x) {
                        return seekArea.width > 0 ? Math.max(0, Math.min(1, x / seekArea.width)) : 0
                    }
                    Timer {
                        id: scrubSeek
                        interval: 120
                        onTriggered: if (seekArea.scrubbing) veyra.seekTo(seekArea.scrubFrac * veyra.duration)
                    }
                    Timer {
                        id: thumbnailDelay
                        interval: 120
                        onTriggered: seekArea.thumbnailBucket = Math.floor(seekArea.hoverFrac * veyra.duration / 2)
                    }
                    Rectangle {
                        id: rail
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        height: seekMouse.containsMouse ? 6 : 3
                        radius: 9
                        color: Qt.rgba(1, 1, 1, 0.14)
                        Behavior on height { NumberAnimation { duration: Theme.d(300); easing.bezierCurve: Theme.spring } }
                        Rectangle {
                            width: parent.width * seekArea.frac
                            height: parent.height
                            radius: 9
                            color: "#FFFFFF"
                        }
                    }
                    // .seek .thumb: 11px dot, scale 0 -> 1 over .4s --spring on hover.
                    Rectangle {
                        width: 11; height: 11; radius: 5.5
                        color: "#FFFFFF"
                        x: seekArea.frac * seekArea.width - width / 2
                        anchors.verticalCenter: parent.verticalCenter
                        scale: seekMouse.containsMouse || seekArea.scrubbing ? 1 : 0
                        Behavior on scale { NumberAnimation { duration: Theme.d(400); easing.bezierCurve: Theme.spring } }
                    }
                    // .seek .peek: 144x104 above the rail, opacity .15s, scale .85 -> 1
                    // over .4s --spring from its bottom centre.
                    Rectangle {
                        visible: opacity > 0.01
                        width: 144
                        height: 104
                        radius: 8
                        color: "#000000"
                        border.width: 1
                        border.color: Theme.stroke2
                        x: Math.max(0, Math.min(parent.width - width,
                                                seekArea.hoverFrac * seekArea.width - width / 2))
                        y: -height - 18
                        opacity: seekMouse.containsMouse || seekArea.scrubbing ? 1 : 0
                        scale: seekMouse.containsMouse || seekArea.scrubbing ? 1 : 0.85
                        transformOrigin: Item.Bottom
                        Behavior on opacity { NumberAnimation { duration: Theme.d(150) } }
                        Behavior on scale { NumberAnimation { duration: Theme.d(400); easing.bezierCurve: Theme.spring } }
                        Image {
                            id: previewFrame
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            height: 81
                            asynchronous: true
                            cache: true
                            fillMode: Image.PreserveAspectFit
                            source: (seekMouse.containsMouse || seekArea.scrubbing) && seekArea.thumbnailBucket >= 0
                                    ? "image://veyra-thumb/" + veyra.thumbnailGeneration + "/" + (seekArea.thumbnailBucket * 2000)
                                    : ""
                        }
                        Text {
                            anchors.centerIn: previewFrame
                            visible: previewFrame.status === Image.Loading || previewFrame.status === Image.Error
                            text: previewFrame.status === Image.Loading ? qsTr("加载中") : qsTr("预览不可用")
                            color: Theme.t3
                            font.family: Theme.fontUi
                            font.pixelSize: 11
                        }
                        Text {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 23
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            text: veyra.formatTime(seekArea.hoverFrac * veyra.duration)
                            color: Theme.t1
                            font.family: Theme.fontMono
                            font.pixelSize: 11
                            font.weight: Font.Medium; font.variableAxes: Theme.axesMedium
                            Rectangle {
                                anchors.fill: parent
                                z: -1
                                color: "#111111"
                            }
                        }
                    }
                    MouseArea {
                        id: seekMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        preventStealing: true
                        enabled: veyra.duration > 0 && !veyra.isCapture
                        cursorShape: Qt.PointingHandCursor
                        onEntered: thumbnailDelay.restart()
                        onExited: {
                            thumbnailDelay.stop()
                            seekArea.thumbnailBucket = -1
                        }
                        onPressed: mouse => {
                            seekArea.scrubFrac = seekArea.fractionAt(mouse.x)
                            seekArea.scrubbing = true
                        }
                        onPositionChanged: mouse => {
                            if (seekArea.scrubbing) {
                                seekArea.scrubFrac = seekArea.fractionAt(mouse.x)
                                // Scrubbing seeks as it goes, 120 ms apart.
                                if (!scrubSeek.running) scrubSeek.start()
                            }
                            thumbnailDelay.restart()
                        }
                        onReleased: mouse => {
                            const target = seekArea.fractionAt(mouse.x) * veyra.duration
                            scrubSeek.stop()
                            seekArea.shownProgress = seekArea.fractionAt(mouse.x)
                            seekArea.scrubbing = false
                            veyra.logUi("ui-seek", "targetSeconds=" + target.toFixed(3))
                            veyra.seekTo(target)
                        }
                        onCanceled: seekArea.scrubbing = false
                    }
                }
        Text {
            objectName: "minimal-duration"
            text: veyra.durationText; color: Theme.t2
            font.family: Theme.fontMono; font.pixelSize: Theme.fsSmall
        }
    }
    Text {
        visible: bar.browserSource
        anchors.left: parent.left; anchors.right: actions.left
        anchors.leftMargin: 12; anchors.rightMargin: 12
        height: 48; verticalAlignment: Text.AlignVCenter
        text: veyra.browserTelemetry.live ? qsTr("浏览器 · 实时监控") : qsTr("浏览器 · 已冻结")
        color: Theme.t3; font.family: Theme.fontUi; font.pixelSize: Theme.fsSmall; elide: Text.ElideRight
    }
    RowLayout {
        id: actions
        objectName: "minimal-actions"
        anchors.right: parent.right; anchors.rightMargin: 12
        y: bar.compact ? 40 : 8; height: 32; spacing: 10
        PlaybackRateButton {
            id: playbackRate; objectName: "minimal-playback-rate"
            visible: !bar.browserSource; menuBottomLimit: bar.menuBottomLimit
        }
        VTag {
            objectName: "minimal-fg-tag"
            visible: veyra.hasSource
            kind: veyra.fgEnabled ? "acc" : ""
            text: veyra.fgEnabled ? veyra.fgMultiplier + "X" : qsTr("补帧关")
        }
        BarButton {
            id: ccButton; objectName: "minimal-subtitles"; glyph: "cc"; tip: qsTr("字幕")
            visible: !bar.browserSource && !veyra.isCapture
            onTapped: ccMenu.openAt(ccButton, "up")
        }
        BarButton {
            id: audioButton; objectName: "minimal-audio-tracks"; glyph: "music"; tip: qsTr("音轨")
            visible: !bar.browserSource && !veyra.isCapture
            onTapped: audioMenu.openAt(audioButton, "up")
        }
        BarButton {
            objectName: "minimal-fullscreen"; glyph: "max"; tip: qsTr("全屏（F11）")
            visible: !bar.browserSource
            onTapped: bar.requestFullscreen()
        }
    }
    // 字幕 (design: app.menu(anchor, '字幕', [...])): the file's own tracks
    // (embedded and same-name external) from the subtitle loader, the primary
    // one checked, then loading a file and the settings dialog.
    VMenu {
        id: ccMenu
        objectName: "minimal-subtitle-menu"
        height: bar.menuBottomLimit >= 0 ? Math.min(implicitHeight, Math.max(72, bar.menuBottomLimit - 8)) : implicitHeight
        aboveLimit: bar.menuBottomLimit
        title: qsTr("字幕")
        items: [{ label: qsTr("关闭", "off"), checked: veyra.subtitlePrimary < 0, track: -1 }]
            .concat(veyra.subtitleTracks.map(t => ({ label: t.label, note: t.note, track: t.index,
                                                     disabled: !t.usable, checked: t.index === veyra.subtitlePrimary })))
            .concat([{ label: qsTr("在线字幕…"), icon: "globe", act: "online", disabled: !veyra.onlineSubtitles.available },
                     { label: qsTr("加载外部字幕…"), icon: "import", act: "load" },
                     { sep: true },
                     { label: qsTr("字幕设置…"), note: qsTr("字体、字号、描边、位置、延时"), icon: "type", act: "dlg" }])
        onPicked: (i, o) => {
            if (o.act === "online") bar.requestDialog("online-subtitle")
            else if (o.act === "load") veyra.loadSubtitleDialog()
            else if (o.act === "dlg") bar.requestDialog("subtitle")
            else if (o.track !== undefined) veyra.subtitlePrimary = o.track
        }
    }
    // 音轨: this one is real - the bridge exposes the file's audio streams and the
    // selected index, so the list and the check mark both come from the engine.
    VMenu {
        id: audioMenu
        objectName: "minimal-audio-menu"
        height: bar.menuBottomLimit >= 0 ? Math.min(implicitHeight, Math.max(72, bar.menuBottomLimit - 8)) : implicitHeight
        aboveLimit: bar.menuBottomLimit
        title: qsTr("音轨")
        // The bridge's label already carries language · title · codec; only the
        // channel count is separate.
        readonly property var mk: t => ({ label: t.label, index: t.index,
                                          note: t.channels > 0 ? (t.channels + qsTr(" 声道")) : "",
                                          checked: t.index === veyra.selectedAudioTrack })
        items: veyra.audioTracks.length > 0
               ? veyra.audioTracks.map(mk).concat([{ sep: true },
                     { label: qsTr("音频设置…"), note: qsTr("输出设备、音画同步、偏移"), icon: "music", act: "dlg" }])
               : [{ label: qsTr("片源没有音轨或尚未打开"), disabled: true }]
        onPicked: (i, o) => {
            if (o.act === "dlg") return bar.requestDialog("audio")
            if (o.index !== undefined) veyra.selectedAudioTrack = o.index
        }
    }
}
