// 登录界面只接收上游文档允许公开的诊断摘要，不保留原始请求或凭据。
export const QQ_DEVICE_NOTICE =
    'QQ 音乐账号只允许本人 7 天内在最多 5 台设备上登录，如登录失败，请在设备管理中删除无用设备。';

export function safeLoginDetails(body, includePrevious = true) {
    const result = {};
    for (const name of ['failureStage', 'failureReason']) {
        if (typeof body?.[name] === 'string' && /^[a-z-]{1,64}$/.test(body[name])) {
            result[name] = body[name];
        }
    }
    for (const name of [
        'retryAfterMs',
        'upstreamHttpStatus',
        'upstreamCode',
        'upstreamGlobalCode',
        'upstreamSubCode',
    ]) {
        if (typeof body?.[name] === 'number' && Number.isFinite(body[name])) {
            result[name] = body[name];
        }
    }
    if (includePrevious && body?.lastFailure) {
        result.lastFailure = safeLoginDetails(body.lastFailure, false);
    }
    return result;
}

export function loginFailureMessage(details) {
    const failure = details.lastFailure || details;
    if (failure.upstreamCode === 20279) {
        return '登录设备数量已达上限，请在 QQ 音乐设备管理中删除无用设备';
    }
    if (failure.failureReason === 'qr-timeout') {
        return '二维码已过期，请重新生成';
    }
    if (failure.failureReason === 'user-canceled') {
        return '已取消扫码登录';
    }
    if (failure.failureStage === 'credential-validation') {
        return '登录凭证验证失败，请重试或检查设备管理';
    }
    if (failure.failureStage === 'credential-exchange') {
        return '扫码授权交换失败，请重试或检查设备管理';
    }
    if (failure.failureStage === 'mqtt-listener') {
        return '扫码连接中断，请检查网络后重试';
    }
    if (failure.failureReason === 'session-busy') {
        return '扫码授权仍在处理中，请等待';
    }
    if (details.code === 429 || details.failureReason === 'local-backoff') {
        return '扫码登录暂时冷却';
    }
    if (details.code === 401) {
        return 'QQ 登录已失效，请重新扫码';
    }
    return details.code === 800 ? '二维码会话已失效或登录失败，请重新生成' : '扫码登录失败，请稍后重试';
}
