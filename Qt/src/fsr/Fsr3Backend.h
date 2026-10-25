#pragma once
#include <QSize>
#include <memory>

class QRhi;
class QRhiCommandBuffer;
class QRhiTexture;

/**
 * FSR3 超分主机后端封装。
 *
 * 只在这里接触 FidelityFX 的类型，头文件不暴露 SDK 头，避免把它的 include 路径
 * 扩散到整个项目。当前实现覆盖 Vulkan 与 D3D12 两个后端（FSR3 只支持这两个）。
 *
 * 生命周期与渲染分辨率绑定：窗口或渲染倍率变化时调用 resize() 重建上下文。
 * 每帧在命令缓冲区里调用 dispatch() 记录 FSR3 的派发。
 */
class Fsr3Backend {
public:
    Fsr3Backend();
    ~Fsr3Backend();
    Fsr3Backend(const Fsr3Backend &) = delete;
    Fsr3Backend &operator=(const Fsr3Backend &) = delete;

    /**
     * 编译期是否带入了 FSR3（任一后端）。用于把设置项置灰。
     */
    static bool compiledIn();
    /**
     * 给定 QRhi 的当前后端能否跑 FSR3：只有 Vulkan 与 D3D12 可以。
     * D3D11 与 OpenGL 返回 false——它们要用 FSR1。
     */
    static bool supported(QRhi *rhi);

    /** 后端是否已就绪（非 Vulkan/D3D12 或驱动不满足时返回 false）。 */
    bool available() const;
    /** 按渲染尺寸与显示尺寸创建/重建上下文；尺寸未变时是空操作。 */
    bool resize(QRhi *rhi, QSize renderSize, QSize displaySize);
    void release();
    /** 尺寸或相机不连续时置位，下一帧派发会带 reset 标记。 */
    void requestReset();
    /** 相机参数与帧时间，供每帧派发使用。 */
    void setCamera(float nearPlane, float farPlane, float frameTimeMs);
    /** 抖动偏移（像素），需与投影矩阵里的抖动一致。 */
    void setJitter(float x, float y);

    /**
     * 记录一帧 FSR3 超分。
     * color/depth/velocity 是渲染分辨率的输入，output 是显示分辨率的输出。
     * 必须在 QRhiCommandBuffer 的 external 段内调用，并把该命令缓冲区一并传入。
     */
    bool dispatch(QRhi *rhi, QRhiCommandBuffer *commandBuffer, QRhiTexture *color, QRhiTexture *depth,
                  QRhiTexture *velocity, QRhiTexture *output);

private:
    struct Private;
    std::unique_ptr<Private> d;
};
