#include "Hud.h"
#include "Engine.h"
#include <QDebug>
#include <QPainter>
#include <QQuickWindow>
Hud::Hud(QQuickItem *parent) : QQuickPaintedItem(parent) {
    setOpaquePainting(false);
    setAntialiasing(true);
}
void Hud::setEngine(Engine *engine) {
    if (engine == m_engine) {
        return;
    }
    if (m_engine) {
        disconnect(m_engine, nullptr, this, nullptr);
    }
    m_engine = engine;
    if (engine) {
        connect(engine, &Engine::hudUpdated, this, [this] { update(); });
    }
    emit engineChanged();
    update();
}
void Hud::paint(QPainter *painter) {
    if (!m_engine) {
        return;
    }
    // 探针（RHINE_HUD_PROBE=1）：这条覆盖层的真实尺寸与 DPR 决定 HUD 的绘制缩放，
    // 与 NativeHud 记录的舞台宽度必须自洽（舞台宽 = item 宽 / (item 高/1080)）。
    if (qEnvironmentVariableIntValue("RHINE_HUD_PROBE") == 1) {
        static QSizeF lastProbeSize;
        const QSizeF size(width(), height());
        if (size != lastProbeSize) {
            lastProbeSize = size;
            qWarning().noquote() << "HUD 探针: item 尺寸" << size << "DPR"
                                 << (window() ? window()->devicePixelRatio() : 0.0);
        }
    }
    painter->save();
    painter->scale(height() / 1080, height() / 1080);
    m_engine->glyphs()->commands(*painter, m_engine->commands(),
                                 [this](QString key) { return m_engine->host.image(key); });
    painter->restore();
}
