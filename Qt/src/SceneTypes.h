#pragma once
#include <QByteArray>
#include <QHash>
#include <QJsonObject>

struct SceneDrawData {
    QByteArray matrices;
    QByteArray themes;
};
using SceneDrawBuffers = QHash<int, SceneDrawData>;

inline bool isFocusedSceneCard(const QJsonObject &draw, bool detailMode, bool viewer) {
    return detailMode && !draw["instanced"].toBool() && (viewer || draw["musicSelected"].toBool());
}
