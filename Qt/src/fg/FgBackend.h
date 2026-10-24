#pragma once

#include <QSize>
#include <QString>
#include <QVector3D>
#include <memory>

#include <rhi/qrhi.h>

class QRhi;
class QRhiCommandBuffer;
class QRhiTexture;

/**
 * FSR3 帧生成（FG）的算法层：封装 ffxOpticalflow + ffxFrameInterpolation。
 *
 * 关键结论（决定了这套接口为什么长这样）：FfxOpticalflowContextDescription 与
 * FfxFrameInterpolationContextDescription 都没有 swapchain 字段，算法与呈现是
 * 解耦的。所以这里只做"记录计算命令"，交换链与 present 全部交给 VulkanPresenter。
 *
 * 资源契约：
 *  - color / colorHudLess / output 都是显示分辨率，且三者格式必须相同
 *    （FfxFrameInterpolationContextDescription::previousInterpolationSourceFormat
 *    要求与 backBufferFormat 同精度组，SDK 在 create 时会校验）。
 *  - depth / motionVectors 是渲染分辨率；motionVectors 是 RGBA16F 的 NDC 位移，
 *    所以 motionVectorScale 取渲染分辨率（与 Fsr3Backend 的口径一致）。
 *  - 内部共享资源（dilatedDepth / dilatedMotionVectors / reconstructedPrevDepth）
 *    由本类自己分配。原因：ffxFrameInterpolationPrepare 会无条件注册这三个 UAV，
 *    传空资源会让注册失败、后续 pass 拿到空描述符，属于必崩路径。
 *
 * 线程与时机：所有方法都必须在渲染线程调用；resize()/release() 会销毁 GPU 资源，
 * 只能在帧外调用（GPU 空闲时）。dispatch()/prepare() 必须在 QRhiCommandBuffer 的
 * beginExternal()/endExternal() 之间调用。
 */
class FgBackend
{
public:
    FgBackend();
    ~FgBackend();
    FgBackend(const FgBackend &) = delete;
    FgBackend &operator=(const FgBackend &) = delete;

    /// 编译期是否编进了 FG（只有 Vulkan 后端）。
    static bool compiledIn();

    /// 给定 QRhi 的当前后端能否跑 FG：只支持 Vulkan。
    /// DX12 的算法层同样是解耦的，但呈现层要另写一套 IDXGISwapChain3，
    /// 所以这里先明确返回 false，而不是让调用方走到半成品分支。
    static bool supported(QRhi *rhi);

    /// 上下文是否已建立。
    bool available() const;

    /**
     * 按渲染尺寸与显示尺寸创建/重建 OF 与 FI 上下文，以及全部内部资源。
     * colorFormat 是 color/colorHudLess/output 的格式，必须与调用方实际使用的
     * 纹理一致——SDK 会用它在 create 时校验精度组，并在 dispatch 时校验格式。
     * 尺寸与格式都没变时是空操作。
     */
    bool resize(QRhi *rhi, QSize renderSize, QSize displaySize,
                QRhiTexture::Format colorFormat = QRhiTexture::RGBA16F);

    /// 销毁上下文与内部资源。调用前必须保证 GPU 不再使用这些资源。
    void release();

    /// 相机参数。fovYRadians 是弧度；frameTimeMs 是毫秒。
    void setCamera(float nearPlane, float farPlane, float fovYRadians, float frameTimeMs);

    /// 亚像素抖动（像素单位），必须与投影矩阵里的抖动一致。
    void setJitter(float x, float y);

    /// 世界空间相机姿态。SDK 从 1.1.4 起把它当"最佳质量"输入：
    /// 全零时会打印警告，并用退化的相机基重建运动矢量场。默认给一组合理的
    /// 右手系基（上 +Y、右 +X、前 -Z），调用方有真实姿态时应覆盖。
    void setCameraPose(const QVector3D &position, const QVector3D &up,
                       const QVector3D &right, const QVector3D &forward);

    /// 标记相机不连续：下一帧 prepare/dispatch 会带 reset，SDK 会丢弃历史。
    void requestReset();

    /**
     * 跑 optical flow，并跑 FI 的 Prepare（重建+膨胀深度/速度、重建上一帧深度）。
     * colorHudLess 是显示分辨率的无 HUD 颜色，OF 用它估光流，避免 HUD 被当成
     * 真实运动而产生残影。depth / motionVectors 是渲染分辨率的输入。
     */
    bool prepare(QRhi *rhi, QRhiCommandBuffer *commandBuffer, QRhiTexture *colorHudLess,
                 QRhiTexture *depth, QRhiTexture *motionVectors);

    /**
     * 跑 frame interpolation，把插值帧写进 output。
     * output 传空时写进本类在 resize() 里创建的内部纹理（用 output() 取）。
     * color 是显示分辨率、含 HUD 的当前帧；colorHudLess 可空，为空时 SDK 直接
     * 用 color 当插值源（HUDLessAttachedFactor=0）。
     *
     * 注意：返回 true 只代表命令记录成功。当 lastFrameReset() 为 true 时，
     * SDK 跳过了准备 pass，这一帧的插值结果不可用，调用方应当直接呈现 color。
     */
    bool dispatch(QRhi *rhi, QRhiCommandBuffer *commandBuffer, QRhiTexture *color,
                  QRhiTexture *colorHudLess, QRhiTexture *output = nullptr);

    /// 内部插值帧纹理（显示分辨率，格式同 resize 的 colorFormat）。未 resize 时为空。
    QRhiTexture *output() const;
    QSize renderSize() const;
    QSize displaySize() const;

    /// 最近一次 dispatch 是否带了 reset（此时插值结果不可用）。
    bool lastFrameReset() const;

    /// 最近一次失败的可读原因，供上层打日志或决定降级。
    QString lastError() const;

private:
    /// 首帧把内部裸图与输出纹理从 UNDEFINED 转到 GENERAL。
    /// 参数是 VkCommandBuffer，但头文件不引 Vulkan，所以用 void* 传递。
    void recordInitialTransitions(void *commandList);

    struct Private;
    std::unique_ptr<Private> d;
};
