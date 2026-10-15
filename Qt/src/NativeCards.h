#pragma once
#include "SceneRuntime.h"
#include <QJsonArray>

// JS 只维护卡片根状态
class NativeCards {
  public:
    QString expand(QJsonObject &frame, const QByteArray &bytes, SceneRuntime::DrawBuffers &buffers);

  private:
    QJsonArray parts;
};
