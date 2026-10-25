#include "IOSServerTrust.h"
#include <QFutureWatcher>
#include <QJsonArray>
#include <QtConcurrentRun>
#import <Foundation/Foundation.h>
#import <Security/Security.h>

namespace {
/** 只使用系统信任库和 SSL 服务端策略，包含用户已明确启用的根证书信任。 */
bool verifyServer(const QJsonObject &request) {
    @autoreleasepool {
        const auto hostname = request["hostname"].toString();
        const auto chain = request["certificates"].toArray();
        if (hostname.isEmpty() || hostname.size() > 253 || chain.isEmpty() || chain.size() > 16) {
            return false;
        }
        NSMutableArray *certificates = [NSMutableArray array];
        for (const auto &encoded : chain) {
            const auto bytes = QByteArray::fromBase64(encoded.toString().toLatin1());
            if (bytes.isEmpty() || bytes.size() > 1024 * 1024) {
                return false;
            }
            NSData *data = [NSData dataWithBytes:bytes.constData() length:bytes.size()];
            SecCertificateRef certificate = SecCertificateCreateWithData(nullptr, (__bridge CFDataRef)data);
            if (!certificate) {
                return false;
            }
            [certificates addObject:CFBridgingRelease(certificate)];
        }
        NSString *host = [NSString stringWithUTF8String:hostname.toUtf8().constData()];
        SecPolicyRef policy = SecPolicyCreateSSL(true, (__bridge CFStringRef)host);
        SecTrustRef trust = nullptr;
        const auto status = SecTrustCreateWithCertificates((__bridge CFArrayRef)certificates, policy, &trust);
        CFRelease(policy);
        if (status != errSecSuccess || !trust) {
            return false;
        }
        // 系统可以获取缺失的中间证书；不添加自定义锚点，也不覆盖用户的信任设置。
        SecTrustSetNetworkFetchAllowed(trust, true);
        const bool trusted = SecTrustEvaluateWithError(trust, nullptr);
        CFRelease(trust);
        return trusted;
    }
}
}

/** 系统链验证可能访问网络，在后台执行；结果只回到当前 Qt 运行时。 */
void evaluateIOSServerTrust(QObject *context, const QJsonObject &request, std::function<void(bool)> reply) {
    auto watcher = new QFutureWatcher<bool>(context);
    QObject::connect(watcher, &QFutureWatcher<bool>::finished, context, [watcher, reply] {
        const bool trusted = watcher->result();
        watcher->deleteLater();
        reply(trusted);
    });
    watcher->setFuture(QtConcurrent::run([request] { return verifyServer(request); }));
}
