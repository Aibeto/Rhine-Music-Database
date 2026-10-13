import QtQuick
import QtQuick.Window
import Rhine.Native 1.0

Window {
    id: root
    width: 1280
    height: 720
    minimumWidth: 960
    minimumHeight: 540
    visible: true
    flags: Qt.Window | Qt.WindowTitleHint | Qt.WindowSystemMenuHint | Qt.WindowMinMaxButtonsHint | Qt.WindowCloseButtonHint | Qt.WindowFullscreenButtonHint
    property int windowedVisibility: Window.Windowed
    // 退出全屏时恢复进入前的窗口模式，保留窗口尺寸与场景坐标换算。
    function toggleFullscreen() {
        if (visibility === Window.FullScreen) {
            visibility = windowedVisibility;
        } else {
            windowedVisibility = visibility === Window.Maximized ? Window.Maximized : Window.Windowed;
            showFullScreen();
        }
    }
    title: "RHINE MUSIC DATABASE"
    color: "#e7e7e1"
    property real stageScale: height / 1080
    property real stageWidth: width / stageScale
    onVisibilityChanged: function (visibility) {
        rhine.setVisible(visible && visibility !== Window.Minimized);
    }
    onWidthChanged: rhine.resize(width, height)
    onHeightChanged: rhine.resize(width, height)
    Component.onCompleted: {
        rhine.resize(width, height);
        inputLayer.forceActiveFocus();
    }
    Shortcut {
        sequences: [StandardKey.Quit]
        onActivated: Qt.quit()
    }
    Shortcut {
        sequences: Qt.platform.os === "osx" ? [StandardKey.FullScreen, "F11"] : ["F11"]
        onActivated: root.toggleFullscreen()
    }
    Connections {
        target: rhine
        function onFullscreenRequested() {
            root.toggleFullscreen();
        }
    }
    RhiScene {
        anchors.fill: parent
        engine: rhine
    }
    Hud {
        anchors.fill: parent
        engine: rhine
    }
    // 透明 QML 输入层使用原舞台坐标；可见画面由同一份字形和材质绘制。
    MouseArea {
        id: inputLayer
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        focus: true
        onPressed: function (mouse) {
            forceActiveFocus();
            rhine.pointer("pointerdown", mouse.x / root.stageScale, mouse.y / root.stageScale);
        }
        onPositionChanged: function (mouse) {
            rhine.pointer("pointermove", mouse.x / root.stageScale, mouse.y / root.stageScale);
        }
        onReleased: function (mouse) {
            rhine.pointer("pointerup", mouse.x / root.stageScale, mouse.y / root.stageScale);
        }
        onExited: rhine.pointer("pointerleave", 0, 0)
        onCanceled: rhine.pointer("pointercancel", 0, 0)
        onWheel: function (wheel) {
            var delta = wheel.pixelDelta.y !== 0 ? wheel.pixelDelta.y : wheel.angleDelta.y / 8;
            rhine.wheel(wheel.x / root.stageScale, wheel.y / root.stageScale, delta, wheel.pixelDelta.y !== 0);
            wheel.accepted = true;
        }
        Keys.onPressed: function (event) {
            var names = {};
            names[Qt.Key_Left] = "ArrowLeft";
            names[Qt.Key_Right] = "ArrowRight";
            names[Qt.Key_Up] = "ArrowUp";
            names[Qt.Key_Down] = "ArrowDown";
            names[Qt.Key_Return] = "Enter";
            names[Qt.Key_Enter] = "Enter";
            names[Qt.Key_Escape] = "Escape";
            if (event.key === Qt.Key_MediaTogglePlayPause || event.key === Qt.Key_MediaPlay || event.key === Qt.Key_MediaPause) {
                rhine.action("player:toggle");
                event.accepted = true;
                return;
            }
            if (event.key === Qt.Key_MediaNext) {
                rhine.action("player:next");
                event.accepted = true;
                return;
            }
            if (event.key === Qt.Key_MediaPrevious) {
                rhine.action("player:prev");
                event.accepted = true;
                return;
            }
            var key = names[event.key] || event.text;
            if (key) {
                // Qt 在 macOS 将 ControlModifier 映射到 ⌘。
                rhine.key(key, (event.modifiers & Qt.ControlModifier) !== 0);
                event.accepted = true;
            }
        }
    }
    Item {
        id: controls
        width: root.stageWidth
        height: 1080
        scale: root.stageScale
        transformOrigin: Item.TopLeft
        Repeater {
            model: rhine.hits
            delegate: Item {
                required property var modelData
                x: modelData.x
                y: modelData.y + (rhine.state.modal && !modelData.fixed ? rhine.state.modalOffset || 0 : 0)
                width: modelData.width
                height: modelData.height
                Accessible.role: Accessible.Button
                Accessible.name: modelData.label
                Accessible.onPressAction: rhine.action(modelData.action)
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onEntered: rhine.pointer("pointerleave", 0, 0)
                    onPressed: {
                        inputLayer.forceActiveFocus();
                        rhine.action(modelData.action);
                    }
                    onWheel: function (wheel) {
                        var p = mapToItem(inputLayer, wheel.x, wheel.y);
                        rhine.wheel(p.x / root.stageScale, p.y / root.stageScale, wheel.pixelDelta.y !== 0 ? wheel.pixelDelta.y : wheel.angleDelta.y / 8, wheel.pixelDelta.y !== 0);
                        wheel.accepted = true;
                    }
                }
            }
        }
    }
    // 两个拖动区域保持固定对象，避免每帧更新命中列表时丢失鼠标抓取。
    Item {
        anchors.fill: parent
        Repeater {
            model: 2
            delegate: MouseArea {
                required property int index
                property var bounds: index === 0 ? rhine.state.seek : rhine.state.volume
                visible: !!bounds
                x: (bounds ? bounds.x : 0) * root.stageScale
                y: (bounds ? bounds.y : 0) * root.stageScale
                width: (bounds ? bounds.width : 0) * root.stageScale
                height: (bounds ? bounds.height : 0) * root.stageScale
                cursorShape: Qt.PointingHandCursor
                function applyPosition(value) {
                    if (width > 0) {
                        rhine.action((index === 0 ? "player:seek-fraction:" : "player:volume-fraction:") + Math.max(0, Math.min(1, value / width)));
                    }
                }
                onPressed: function (mouse) {
                    applyPosition(mouse.x);
                }
                onPositionChanged: function (mouse) {
                    if (pressed) {
                        applyPosition(mouse.x);
                    }
                }
                onReleased: function (mouse) {
                    applyPosition(mouse.x);
                }
            }
        }
    }
    // 密码只在输入组件和单次登录请求中短暂存在，提交后立即清空。
    Item {
        id: formFields
        visible: rhine.state.modal === "playlist-form" || (rhine.state.modal === "login" && ["phone", "email"].indexOf(rhine.state.loginMethod) >= 0)
        x: (rhine.state.inputX || 0) * root.stageScale
        y: (rhine.state.modalOffset || 0) * root.stageScale
        width: (rhine.state.inputWidth || 0) * root.stageScale
        height: root.height
        opacity: rhine.state.modalOpacity || 0
        onVisibleChanged: {
            accountField.text = "";
            passwordField.text = "";
            if (visible) {
                accountField.forceActiveFocus();
            }
        }
        function submit() {
            var data = (rhine.state.modal !== "login") ? {
                name: accountField.text
            } : {
                account: accountField.text,
                password: passwordField.text
            };
            rhine.form(JSON.stringify(data));
            passwordField.text = "";
        }
        Rectangle {
            x: 0
            y: 430 * root.stageScale
            width: parent.width
            height: 55 * root.stageScale
            color: rhine.state.dark ? "#30312b" : "#d9d8ce"
        }
        TextInput {
            id: accountField
            objectName: "accountField"
            x: 14 * root.stageScale
            y: 430 * root.stageScale
            width: parent.width - 28 * root.stageScale
            height: 55 * root.stageScale
            verticalAlignment: TextInput.AlignVCenter
            color: rhine.state.dark ? "#eeeeeb" : "#161914"
            font.pixelSize: 19 * root.stageScale
            cursorVisible: activeFocus && !rhine.performance.idle
            maximumLength: 100
            Accessible.name: rhine.state.modal === "playlist-form" ? "歌单名称" : "登录账号"
            Keys.onReturnPressed: {
                if (rhine.state.modal !== "login") {
                    formFields.submit();
                } else {
                    passwordField.forceActiveFocus();
                }
            }
            Keys.onEscapePressed: rhine.action("close-modal")
        }
        Rectangle {
            visible: rhine.state.modal === "login"
            x: 0
            y: 541 * root.stageScale
            width: parent.width
            height: 55 * root.stageScale
            color: rhine.state.dark ? "#30312b" : "#d9d8ce"
        }
        TextInput {
            id: passwordField
            objectName: "passwordField"
            visible: rhine.state.modal === "login"
            x: 14 * root.stageScale
            y: 541 * root.stageScale
            width: parent.width - 28 * root.stageScale
            height: 55 * root.stageScale
            verticalAlignment: TextInput.AlignVCenter
            color: rhine.state.dark ? "#eeeeeb" : "#161914"
            font.pixelSize: 19 * root.stageScale
            cursorVisible: activeFocus && !rhine.performance.idle
            maximumLength: 256
            echoMode: TextInput.Password
            Accessible.name: "密码"
            Keys.onReturnPressed: formFields.submit()
            Keys.onEscapePressed: rhine.action("close-modal")
        }
        Rectangle {
            x: parent.width - width
            y: (rhine.state.modal === "login" ? 611 : 530) * root.stageScale
            width: 170 * root.stageScale
            height: 36 * root.stageScale
            color: "#777b60"
            Text {
                anchors.centerIn: parent
                text: rhine.state.modal === "login" ? "登录 →" : "保存 →"
                color: "#f0eee5"
                font.pixelSize: 14 * root.stageScale
            }
            Accessible.role: Accessible.Button
            Accessible.name: "提交表单"
            Accessible.onPressAction: formFields.submit()
            MouseArea {
                anchors.fill: parent
                onClicked: formFields.submit()
            }
        }
    }
    Repeater {
        model: ["seek", "volume"]
        delegate: MouseArea {
            required property string modelData
            property var region: rhine.state[modelData]
            visible: !!region
            enabled: visible && !rhine.state.closing
            x: region ? region.x * root.stageScale : 0
            y: region ? (region.y + (rhine.state.modal ? rhine.state.modalOffset || 0 : 0)) * root.stageScale : 0
            width: region ? region.width * root.stageScale : 0
            height: region ? region.height * root.stageScale : 0
            cursorShape: Qt.PointingHandCursor
            onPressed: function (mouse) {
                rhine.action("player:" + modelData + "-fraction:" + Math.max(0, Math.min(1, mouse.x / width)));
            }
            onPositionChanged: function (mouse) {
                if (pressed) {
                    rhine.action("player:" + modelData + "-fraction:" + Math.max(0, Math.min(1, mouse.x / width)));
                }
            }
        }
    }
    Connections {
        target: rhine
        property string lastMethod: ""
        function onStateChanged() {
            if (lastMethod !== rhine.state.loginMethod) {
                lastMethod = rhine.state.loginMethod;
                accountField.text = "";
                passwordField.text = "";
            }
        }
    }
    Rectangle {
        visible: searchInput.visible
        x: searchInput.x
        y: searchInput.y
        width: searchInput.width
        height: searchInput.height
        color: rhine.state.dark ? "#ffffff" : "#141613"
        opacity: (rhine.state.modalOpacity || 0) * 0.045
    }
    TextInput {
        id: searchInput
        objectName: "musicSearchInput"
        verticalAlignment: TextInput.AlignVCenter
        leftPadding: 14 * root.stageScale
        rightPadding: 14 * root.stageScale
        renderType: Text.NativeRendering
        visible: rhine.state.modal === "search" || rhine.state.modal === "saved"
        enabled: !rhine.state.closing
        x: (rhine.state.inputX || 0) * root.stageScale
        y: ((rhine.state.inputY || 302) + (rhine.state.modalOffset || 0)) * root.stageScale
        width: (rhine.state.inputWidth || 0) * root.stageScale
        height: 54 * root.stageScale
        font.family: rhineFontFamily
        font.pixelSize: 22 * root.stageScale
        color: rhine.state.dark ? "#ebebeb" : "#141613"
        opacity: rhine.state.modalOpacity || 0
        clip: true
        cursorVisible: activeFocus && !rhine.performance.idle
        selectByMouse: true
        Accessible.name: "搜索歌曲、歌手、专辑或歌单"
        onTextEdited: rhine.query(text)
        onVisibleChanged: {
            if (visible) {
                text = rhine.state.query || "";
                forceActiveFocus();
            } else
                inputLayer.forceActiveFocus();
        }
        Keys.onEscapePressed: rhine.action("close-modal")
        Keys.onReturnPressed: function (event) {
            if ((event.modifiers & Qt.ControlModifier) !== 0) {
                rhine.key("Enter", true);
            } else {
                rhine.query(text);
            }
        }
        Text {
            anchors.fill: parent
            visible: searchInput.text.length === 0 && !searchInput.preeditText
            text: "搜索歌曲、歌手、专辑或歌单"
            font: searchInput.font
            leftPadding: searchInput.leftPadding
            verticalAlignment: Text.AlignVCenter
            color: "#777777"
        }
    }
}
