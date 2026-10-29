import QtQuick
import QtQuick.Window

// 性能模式挂起页：只显示预渲染的启动画面，不依赖业务引擎或任何动画。
// 由 PresentationShell 作为唯一的 UI 离屏层载入；图片不透明，合成时完全覆盖底图。
Item {
    id: page
    anchors.fill: parent

    Rectangle {
        anchors.fill: parent
        color: "#e7e7e1"
    }
    Image {
        anchors.fill: parent
        source: "welcome.png"
        fillMode: Image.PreserveAspectCrop
        asynchronous: false
        sourceSize.width: Math.max(1280, Math.min(3840, Math.round(page.width * Screen.devicePixelRatio)))
    }
}
