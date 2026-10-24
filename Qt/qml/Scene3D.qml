import QtQuick
import Rhine.Native 1.0

// 3D 层：只放 RhiScene 与增强场景，不含 HUD、输入层和任何面板。
// FG 需要这一层按渲染帧率单独出图并做帧间插值；它一旦和 UI 混在同一张纹理里，
// 插值就会把文字和按钮一起算出鬼影。所以这里的边界必须与 Overlay.qml 严格对应：
// 凡是"每帧随 3D 变化"的内容留在本文件，凡是"叠加在 3D 之上"的内容去 Overlay.qml。
//
// 依赖说明：rhine 是引擎根上下文属性（main.cpp:302），不是 Window 专有属性，
// 因此两个宿主（默认的 QQuickWindow 与 FG 的 QQuickRenderControl）共用同一个
// QQmlEngine 时都能解析，无需再注入。这是刻意保留的，避免把 Engine 指针
// 在 QML 树里多传一层。
Item {
    id: scene3d
    objectName: "scene3dRoot"

    RhiScene {
        id: originalScene
        anchors.fill: parent
        engine: rhine
        // 增强画布完全遮住底层；原版只接收预热和取出唱片真正需要的更新。
        visible: true
    }
    Loader {
        id: sceneEnhancementLoader
        objectName: "sceneEnhancementLoader"
        anchors.fill: parent
        active: rhine.state.sceneEnhancement === true
        sourceComponent: OpticalScene {
            objectName: "enhancementComposite"
            engine: rhine
            originalSource: originalScene
        }
    }
}
