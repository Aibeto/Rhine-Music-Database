#pragma once
#include <QJsonObject>
#include <functional>

class QObject;

void evaluateIOSServerTrust(QObject *context, const QJsonObject &request, std::function<void(bool)> reply);
