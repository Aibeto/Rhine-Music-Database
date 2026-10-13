#include "SceneRuntime.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>
#include <cstring>
/**
 * 把原场景运行时接入 QJSEngine，保留缓冲区、资源和输入接口。
 */
SceneRuntime::SceneRuntime(QString root, QJSEngine *qml) : m_qml(qml) {
    auto read = [&](QString name) {
        QFile f(root + "/" + name);
        if (!f.open(QIODevice::ReadOnly)) {
            qFatal("Missing scene resource: %s", qPrintable(name));
        }
        return QString::fromUtf8(f.readAll());
    };
    evaluate("var globalThis=this;var console={log:function(){},warn:function(){},error:function(){}};var "
             "nativeGeometry={};Array.prototype.at=function(i){i=Math.trunc(i)||0;return "
             "this[i<0?this.length+i:i];};");
    // 大模型数据由 JSON 解析器读取，避免把数百万个坐标编译为脚本字面量。
    auto geometry = context.newObject();
    auto parse = context.globalObject().property("JSON").property("parse");
    geometry.setProperty("cassette", parse.call({read("archive-cassette-geometry.json")}));
    geometry.setProperty("assembly", parse.call({read("archive-assembly-geometry.json")}));
    context.globalObject().setProperty("nativeGeometry", geometry);
    evaluate("Object.fromEntries=function(entries){var result={};for(var pair of "
             "entries)result[pair[0]]=pair[1];return "
             "result;};Array.prototype.flat=function(depth){depth=depth===undefined?1:depth;return "
             "this.reduce(function(out,value){return "
             "out.concat(Array.isArray(value)&&depth>0?value.flat(depth-1):value);},[]);};");
    evaluate(read("runtime.js"));
    runtime = context.globalObject().property("RhineNative");
    evaluate("RhineNative.init().catch(function(e){throw e;});");
}
SceneRuntime::~SceneRuntime() = default;
QJSValue SceneRuntime::evaluate(QString code) {
    auto result = context.evaluate(code);
    if (result.isError()) {
        failure = result.toString() + "\n" + result.property("stack").toString();
        qWarning().noquote() << failure;
    }
    return result;
}
QJSValue SceneRuntime::call(QString name, QVariantList values) {
    if (name != "frameFast" && name != "bufferBytes" && name != "buffer" && name != "selection" &&
        name != "pick" && name != "energyIdle") {
        m_changed = true;
    }
    QJSValueList arguments;
    for (auto value : values) {
        arguments << context.toScriptValue(value);
    }
    auto result = runtime.property(name).callWithInstance(runtime, arguments);
    if (result.isError()) {
        failure = result.toString() + "\n" + result.property("stack").toString();
        qWarning().noquote() << name << failure;
    }
    return result;
}
QString SceneRuntime::json(QJSValue value) {
    return context.globalObject().property("JSON").property("stringify").call({value}).toString();
}
bool SceneRuntime::ready() const {
    if (!runtime.isObject()) {
        return false;
    }
    return runtime.property("ready").toBool();
}
QJSValue SceneRuntime::frameFast(double time) {
    auto value = call("frameFast", {time, m_diagnostics});
    m_changed = false;
    if (value.isNull() || value.isUndefined()) {
        return QJSValue(QJSValue::NullValue);
    }
    frame = QJsonDocument::fromJson(json(value).toUtf8()).object();
    auto draws = frame["draws"].toArray();
    QHash<quint64, QByteArray> buffers;
    drawBuffers.clear();
    auto data = [&](int index, int count) {
        if (index < 0 || count <= 0) {
            return QByteArray();
        }
        const quint64 key = (quint64(quint32(index)) << 32) | quint32(count);
        if (!buffers.contains(key)) {
            // ArrayBuffer 一次转换为原生字节，避免对每个 Float32 属性跨引擎读取。
            buffers[key] = context.fromScriptValue<QByteArray>(call("bufferBytes", {index, count}));
        }
        return buffers[key];
    };
    auto numbers = [](const QByteArray &bytes) {
        QJsonArray values;
        for (qsizetype i = 0; i + 4 <= bytes.size(); i += 4) {
            float value;
            memcpy(&value, bytes.constData() + i, sizeof(value));
            values.append(double(value));
        }
        return values;
    };
    for (int i = 0; i < draws.size(); ++i) {
        auto d = draws[i].toObject();
        DrawData bytes{data(d["matrixBuffer"].toInt(-1), d["count"].toInt() * 16),
                       data(d["themeBuffer"].toInt(-1), d["count"].toInt())};
        drawBuffers.insert(d["id"].toInt(), bytes);
        if (m_diagnostics) {
            d["matrices"] = numbers(bytes.matrices);
            d["themes"] = numbers(bytes.themes);
            draws[i] = d;
        }
    }
    frame["draws"] = draws;
    auto summary = frame;
    summary.remove("draws");
    return m_qml->toScriptValue(summary.toVariantMap());
}

void SceneRuntime::setMode(QString value) {
    call("setMode", {value});
}
void SceneRuntime::navigate(QString axis, int direction) {
    call("navigate", {axis, direction});
}
void SceneRuntime::select(int index, QJSValue navigation) {
    if (navigation.isUndefined()) {
        call("select", {index});
    } else {
        call("select", {index, navigation.toVariant(QJSValue::ConvertJSObjects)});
    }
}
// 退出账号时清除整个阵列与查看器的曲目标记，避免旧账户封面继续绑定。
void SceneRuntime::musicClear() {
    call("musicClear", {});
}
void SceneRuntime::musicIdentity(QString key) {
    call("musicIdentity", {key});
}
QJSValue SceneRuntime::pick(double x, double y) {
    auto value = call("pick", {x, y});
    if (value.isNull() || value.isUndefined()) {
        return QJSValue(QJSValue::NullValue);
    }
    return m_qml->toScriptValue(QJsonDocument::fromJson(json(value).toUtf8()).object().toVariantMap());
}
int SceneRuntime::selection() {
    return call("selection").toInt();
}
void SceneRuntime::pointer(QString type, double x, double y, double delta) {
    call("pointer", {type, x, y, delta});
}
void SceneRuntime::resize(double w, double h) {
    call("resize", {w, h});
}
void SceneRuntime::theme(bool dark) {
    call("theme", {dark});
}
void SceneRuntime::reduced(bool value) {
    call("reduced", {value});
}
void SceneRuntime::openViewer() {
    call("openViewer");
}
void SceneRuntime::coverFront() {
    call("coverFront");
}
void SceneRuntime::coverBack() {
    call("coverBack");
}
void SceneRuntime::viewerAction(QString action) {
    call("viewerAction", {action});
}
void SceneRuntime::viewerKey(QString key) {
    call("viewerKey", {key});
}
bool SceneRuntime::review(QString mode) {
    call("review", {mode});
    QElapsedTimer wait;
    wait.start();
    while (!ready() && failure.isEmpty() && wait.elapsed() < 10000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return ready();
}
void SceneRuntime::freeze(QJSValue time) {
    call("freeze", {time.isNull() ? QVariant() : time.toVariant()});
}
