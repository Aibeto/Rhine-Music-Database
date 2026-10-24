import QtQuick
import QtQuick.Window
import Rhine.Native 1.0

// UI 叠加层：HUD、透明输入层、表单、搜索框、窗口边框与性能面板。
// FG 下这一层按显示帧率重新叠加在真实帧和插值帧之上（插值帧只对 3D 有意义），
// 所以它必须与 Scene3D.qml 彻底分离。默认路径下仍由 Main.qml 的 Window 直接承载，
// 层级、坐标与交互保持与拆分前完全一致。
//
// 与原 Main.qml 的差异只有两类，且都不改变行为：
//   1. 原来读 root.stageScale / root.stageWidth / root.height / root.visibility 的地方，
//      改成自身属性或宿主注入（见下面的 stageScale/stageWidth/hostWindow/frameless）。
//   2. 原来被 Main.qml 用 id 直接引用的 inputLayer / accountField / passwordField，
//      现在在组件内部，宿主改为调用 focusInputLayer() / clearCredentials()。
Item {
    id: overlay
    objectName: "overlayRoot"

    // 舞台换算：原来是 Window 上的 root.stageScale 与 root.stageWidth。
    // 默认由 Main.qml 注入；不注入时按自身尺寸回退，这样 FG 宿主只设置尺寸
    // 也能得到同一套舞台坐标。height 为 0 时避免除零（离屏窗口创建到设尺寸之间会短暂为 0）。
    property real stageScale: height > 0 ? height / 1080 : 1
    property real stageWidth: width / stageScale

    // 宿主窗口：WindowChrome 需要它做最小化/最大化/拖拽/边缘缩放。
    // 默认路径由 Main.qml 注入 Window；FG 路径没有可见的 QQuickWindow，保持 null，
    // 下面的 Loader 判空后不创建（期二改成 C++ 注入 startSystemMove/startSystemResize）。
    // 不叫 window 是因为 Item 已有 window 属性（QQuickItem::window()），重名会冲突。
    property Window hostWindow: null
    property bool frameless: false

    // 原 Main.qml 直接引用组件内部的 id，拆层后改为函数调用，效果等价。
    function focusInputLayer() {
        inputLayer.forceActiveFocus();
    }
    function clearCredentials() {
        accountField.text = "";
        passwordField.text = "";
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
            rhine.pointer("pointerdown", mouse.x / overlay.stageScale, mouse.y / overlay.stageScale);
        }
        onPositionChanged: function (mouse) {
            rhine.pointer("pointermove", mouse.x / overlay.stageScale, mouse.y / overlay.stageScale);
        }
        onReleased: function (mouse) {
            rhine.pointer("pointerup", mouse.x / overlay.stageScale, mouse.y / overlay.stageScale);
        }
        onExited: rhine.pointer("pointerleave", 0, 0)
        onCanceled: rhine.pointer("pointercancel", 0, 0)
        onWheel: function (wheel) {
            rhine.wheelInput(wheel.x / overlay.stageScale, wheel.y / overlay.stageScale, wheel.pixelDelta.y, wheel.angleDelta.y);
            wheel.accepted = true;
        }
        Keys.onPressed: function (event) {
            event.accepted = rhine.keyEvent(event.key, event.modifiers, event.text);
        }
    }
    Item {
        id: controls
        width: overlay.stageWidth
        height: 1080
        scale: overlay.stageScale
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
                        rhine.wheelInput(p.x / overlay.stageScale, p.y / overlay.stageScale, wheel.pixelDelta.y, wheel.angleDelta.y);
                        wheel.accepted = true;
                    }
                }
            }
        }
    }
    // 密码只在输入组件和单次登录请求中短暂存在，提交后立即清空。
    Item {
        id: formFields
        visible: rhine.state.modal === "playlist-form" || (rhine.state.modal === "login" && (rhine.state.loginMethod === "phone" || rhine.state.loginMethod === "email"))
        x: (rhine.state.inputX || 0) * overlay.stageScale
        y: (rhine.state.modalOffset || 0) * overlay.stageScale
        width: (rhine.state.inputWidth || 0) * overlay.stageScale
        height: overlay.height
        opacity: rhine.state.modalOpacity || 0
        onVisibleChanged: {
            accountField.text = "";
            passwordField.text = "";
            if (visible) {
                accountField.forceActiveFocus();
            }
        }
        function submit() {
            rhine.submitForm(accountField.text, passwordField.text);
            passwordField.text = "";
        }
        Rectangle {
            x: 0
            y: 430 * overlay.stageScale
            width: parent.width
            height: 55 * overlay.stageScale
            color: rhine.state.dark ? "#30312b" : "#d9d8ce"
        }
        TextInput {
            id: accountField
            objectName: "accountField"
            x: 14 * overlay.stageScale
            y: 430 * overlay.stageScale
            width: parent.width - 28 * overlay.stageScale
            height: 55 * overlay.stageScale
            verticalAlignment: TextInput.AlignVCenter
            color: rhine.state.dark ? "#eeeeeb" : "#161914"
            font.pixelSize: 19 * overlay.stageScale
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
            y: 541 * overlay.stageScale
            width: parent.width
            height: 55 * overlay.stageScale
            color: rhine.state.dark ? "#30312b" : "#d9d8ce"
        }
        TextInput {
            id: passwordField
            objectName: "passwordField"
            visible: rhine.state.modal === "login"
            x: 14 * overlay.stageScale
            y: 541 * overlay.stageScale
            width: parent.width - 28 * overlay.stageScale
            height: 55 * overlay.stageScale
            verticalAlignment: TextInput.AlignVCenter
            color: rhine.state.dark ? "#eeeeeb" : "#161914"
            font.pixelSize: 19 * overlay.stageScale
            cursorVisible: activeFocus && !rhine.performance.idle
            maximumLength: 256
            echoMode: TextInput.Password
            Accessible.name: "密码"
            Keys.onReturnPressed: formFields.submit()
            Keys.onEscapePressed: rhine.action("close-modal")
        }
        Rectangle {
            x: parent.width - width
            y: (rhine.state.modal === "login" ? 611 : 530) * overlay.stageScale
            width: 170 * overlay.stageScale
            height: 36 * overlay.stageScale
            color: "#777b60"
            Text {
                anchors.centerIn: parent
                text: rhine.state.modal === "login" ? "登录 →" : "保存 →"
                color: "#f0eee5"
                font.pixelSize: 14 * overlay.stageScale
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
            x: region ? region.x * overlay.stageScale : 0
            y: region ? (region.y + (rhine.state.modal ? rhine.state.modalOffset || 0 : 0)) * overlay.stageScale : 0
            width: region ? region.width * overlay.stageScale : 0
            height: region ? region.height * overlay.stageScale : 0
            cursorShape: Qt.PointingHandCursor
            onPressed: function (mouse) {
                rhine.adjustPlayer(modelData, mouse.x, width);
            }
            onPositionChanged: function (mouse) {
                if (pressed) {
                    rhine.adjustPlayer(modelData, mouse.x, width);
                }
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
        leftPadding: 14 * overlay.stageScale
        rightPadding: 14 * overlay.stageScale
        renderType: Text.NativeRendering
        visible: rhine.state.modal === "search" || rhine.state.modal === "saved"
        enabled: !rhine.state.closing
        x: (rhine.state.inputX || 0) * overlay.stageScale
        y: ((rhine.state.inputY || 302) + (rhine.state.modalOffset || 0)) * overlay.stageScale
        width: (rhine.state.inputWidth || 0) * overlay.stageScale
        height: 54 * overlay.stageScale
        font.family: rhineFontFamily
        font.pixelSize: 22 * overlay.stageScale
        color: rhine.state.dark ? "#ebebeb" : "#141613"
        opacity: rhine.state.modalOpacity || 0
        clip: true
        cursorVisible: activeFocus && !rhine.performance.idle
        selectByMouse: true
        Accessible.name: rhine.state.localMusic ? "检索本地歌曲、歌手、专辑或文件夹" : "搜索歌曲、歌手、专辑或歌单"
        onTextEdited: rhine.query(text)
        onVisibleChanged: {
            if (visible) {
                text = rhine.state.query || "";
                forceActiveFocus();
            } else {
                inputLayer.forceActiveFocus();
            }
        }
        Keys.onEscapePressed: rhine.action("close-modal")
        Keys.onReturnPressed: function (event) {
            rhine.searchAccepted(text, event.modifiers);
        }
        Text {
            anchors.fill: parent
            visible: searchInput.text.length === 0 && !searchInput.preeditText
            text: rhine.state.localMusic ? "检索本地歌曲、歌手、专辑或文件夹" : "搜索歌曲、歌手、专辑或歌单"
            font: searchInput.font
            leftPadding: searchInput.leftPadding
            verticalAlignment: Text.AlignVCenter
            color: "#777777"
        }
    }

    // WindowChrome 依赖真实 Window（visibility/showMinimized/startSystemMove）。
    // 用 Loader 判空而不是直接实例化：FG 路径没有可见窗口，直接创建会让
    // window.visibility 变成空引用并每帧刷 TypeError。默认路径下 hostWindow 在
    // 创建时就已注入，Loader 同步创建，层级与 z:999 和拆分前一致。
    Loader {
        id: chromeLoader
        anchors.fill: parent
        z: 999
        active: overlay.hostWindow !== null
        sourceComponent: WindowChrome {
            window: overlay.hostWindow
            enabled: overlay.frameless
            visible: enabled && overlay.hostWindow.visibility !== Window.FullScreen
            dark: rhine.state.dark === true
        }
    }
    PerformancePanel {
        z: 1000
    }
}
