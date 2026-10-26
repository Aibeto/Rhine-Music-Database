#include <QDebug>
#include <QtGlobal>
#include <windows.h>
#if RHINE_ENABLE_D3D12_BACKEND
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#endif

extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

#if RHINE_ENABLE_D3D12_BACKEND
using Microsoft::WRL::ComPtr;
#endif

/**
 * Windows 默认使用独立显卡：按 DXGI 的高性能偏好枚举适配器，确认能创建 D3D12 设备后
 * 把索引交给 Qt——QRhiD3D12 读 QT_D3D_ADAPTER_INDEX，Vulkan/OpenGL 会忽略它。
 * 用户已指定索引或要求软件渲染时不覆盖。
 */
void initializeWindowsGraphics() {
#if RHINE_ENABLE_D3D12_BACKEND
    if (qEnvironmentVariableIsSet("QT_D3D_ADAPTER_INDEX") ||
        qEnvironmentVariableIntValue("QSG_RHI_PREFER_SOFTWARE_RENDERER") != 0 ||
        qEnvironmentVariableIntValue("RHINE_SOFTWARE_GPU") == 1) {
        return;
    }

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) {
        qWarning() << "High-performance GPU enumeration unavailable; using Qt default adapter";
        return;
    }

    for (UINT preferenceIndex = 0;; ++preferenceIndex) {
        ComPtr<IDXGIAdapter1> preferredAdapter;
        const auto result =
            factory->EnumAdapterByGpuPreference(preferenceIndex, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                IID_PPV_ARGS(preferredAdapter.GetAddressOf()));
        if (result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(result)) {
            qWarning() << "High-performance GPU enumeration failed" << Qt::hex << result;
            break;
        }

        DXGI_ADAPTER_DESC1 preferredDescription{};
        if (FAILED(preferredAdapter->GetDesc1(&preferredDescription)) ||
            (preferredDescription.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            continue;
        }

        ComPtr<ID3D12Device> probeDevice;
        if (FAILED(D3D12CreateDevice(preferredAdapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                     IID_PPV_ARGS(probeDevice.GetAddressOf())))) {
            continue;
        }

        for (UINT adapterIndex = 0;; ++adapterIndex) {
            ComPtr<IDXGIAdapter1> adapter;
            if (FAILED(factory->EnumAdapters1(adapterIndex, adapter.GetAddressOf()))) {
                break;
            }
            DXGI_ADAPTER_DESC1 description{};
            if (FAILED(adapter->GetDesc1(&description))) {
                continue;
            }
            if (description.AdapterLuid.LowPart == preferredDescription.AdapterLuid.LowPart &&
                description.AdapterLuid.HighPart == preferredDescription.AdapterLuid.HighPart) {
                qputenv("QT_D3D_ADAPTER_INDEX", QByteArray::number(adapterIndex));
                qInfo() << "Selected high-performance D3D12 adapter" << adapterIndex
                        << QString::fromWCharArray(description.Description);
                return;
            }
        }
    }
    qWarning() << "No usable high-performance D3D12 adapter found; using Qt default adapter";
#endif
}
