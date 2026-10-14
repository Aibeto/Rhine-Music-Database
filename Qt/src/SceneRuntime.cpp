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
SceneRuntime::SceneRuntime(QString root, QJSEngine *qml) : m_root(root), m_qml(qml) {
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
    context.globalObject().setProperty("nativeGeometry", geometry);
    QJSEngine::setObjectOwnership(this, QJSEngine::CppOwnership);
    context.globalObject().setProperty("NativeAssets", context.newQObject(this));
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
/**
 * 首次打开查看器时才解析其模型，避免普通音乐浏览长期持有第二份大模型。
 * 固定资产白名单不允许脚本读取任意路径；返回值属于场景 JS 引擎。
 */
QJSValue SceneRuntime::loadGeometry(QString asset) {
    if (asset != "cassette" && asset != "assembly") {
        context.throwError(QStringLiteral("Unknown scene geometry"));
        return QJSValue();
    }
    QFile file(m_root + "/archive-" + asset + "-geometry.json");
    if (!file.open(QIODevice::ReadOnly)) {
        context.throwError(QStringLiteral("Missing scene geometry"));
        return QJSValue();
    }
    return context.globalObject().property("JSON").property("parse").call(
        {QString::fromUtf8(file.readAll())});
}
void SceneRuntime::trimMemory() {
    call("trimMemory");
    context.collectGarbage();
}
QJSValue SceneRuntime::evaluate(QString code) {
    auto result = context.evaluate(code);
    if (result.isError()) {
        failure = result.toString() + "\n" + result.property("stack").toString();
        qWarning().noquote() << failure;
    }
    return result;
}
QJSValue SceneRuntime::call(QString name, QVariantList values) {
    if (name != "frameFast" && name != "framePacked" && name != "packetBytes" && name != "bufferBytes" &&
        name != "buffer" && name != "selection" && name != "pick" && name != "energyIdle") {
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
    QElapsedTimer timer;
    if (m_profileFrames) {
        timer.start();
    }
    auto value =
        m_diagnostics ? call("frameFast", {time, true}) : call("framePacked", {time, !m_captureOnly});
    const auto scriptNs = m_profileFrames ? timer.nsecsElapsed() : 0;
    m_changed = false;
    if (value.isNull() || value.isUndefined()) {
        return QJSValue(QJSValue::NullValue);
    }
    frame = QJsonDocument::fromJson(json(value).toUtf8()).object();
    if (!m_diagnostics) {
        static constexpr int packetStride = 23;
        const auto bytes = context.fromScriptValue<QByteArray>(call("packetBytes"));
        const auto count = frame["packetCount"].toInt();
        if (frame["packetVersion"].toInt() != 1 || count < 0 ||
            bytes.size() != qsizetype(count) * packetStride * 8) {
            failure = QStringLiteral("Invalid native scene packet");
            qWarning().noquote() << failure;
            return QJSValue(QJSValue::NullValue);
        }
        for (const auto &entry : frame["drawDefinitions"].toArray()) {
            const auto definition = entry.toObject();
            m_drawDefinitions[definition["id"].toInt()] = definition;
        }
        QJsonArray decoded;
        QSet<int> activeIDs;
        for (int i = 0; i < count; ++i) {
            double values[packetStride];
            memcpy(values, bytes.constData() + qsizetype(i) * sizeof(values), sizeof(values));
            const auto id = int(values[0]);
            if (!m_drawDefinitions.contains(id)) {
                failure =
                    QStringLiteral("Missing native draw definition: id=%1 row=%2 first=%3 definitions=%4")
                        .arg(id)
                        .arg(i)
                        .arg(values[0])
                        .arg(m_drawDefinitions.size());
                qWarning().noquote() << failure;
                return QJSValue(QJSValue::NullValue);
            }
            auto draw = m_drawDefinitions.value(id);
            activeIDs.insert(id);
            draw["count"] = int(values[1]);
            draw["matrixBuffer"] = int(values[2]);
            draw["themeBuffer"] = int(values[3]);
            draw["color"] = QJsonArray{values[4], values[5], values[6]};
            draw["roughness"] = values[7];
            draw["metalness"] = values[8];
            draw["transmission"] = values[9];
            draw["thickness"] = values[10];
            draw["ior"] = values[11];
            draw["attenuation"] = QJsonArray{values[12], values[13], values[14]};
            draw["attenuationDistance"] = values[15];
            draw["opacity"] = values[16];
            draw["clearcoat"] = values[17];
            draw["clearcoatRoughness"] = values[18];
            draw["quality"] = values[19];
            draw["clarity"] = values[20];
            draw["subdued"] = values[21];
            draw["theme"] = values[22];
            draw["matrices"] = QJsonArray{};
            draw["themes"] = QJsonArray{};
            decoded.append(draw);
        }
        for (auto it = m_drawDefinitions.begin(); it != m_drawDefinitions.end();) {
            if (!activeIDs.contains(it.key())) {
                it = m_drawDefinitions.erase(it);
            } else {
                ++it;
            }
        }
        frame["draws"] = decoded;
        for (const auto &key : {"drawDefinitions", "packetVersion", "packetCount"}) {
            frame.remove(key);
        }
    }
    const auto jsonNs = m_profileFrames ? timer.nsecsElapsed() : 0;
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
    const auto buffersNs = m_profileFrames ? timer.nsecsElapsed() : 0;
    auto result = m_qml->toScriptValue(summary.toVariantMap());
    if (m_profileFrames) {
        timings = {scriptNs / 1000000., (jsonNs - scriptNs) / 1000000., (buffersNs - jsonNs) / 1000000.,
                   (timer.nsecsElapsed() - buffersNs) / 1000000.};
    }
    return result;
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
