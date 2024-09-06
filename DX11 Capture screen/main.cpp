#include <iostream>
#include <iomanip>
#include <chrono>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <Windows.h>
#include <thread>
#include <atomic>
#include <array>
#include <vector>
#include <cmath>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

class DX11 {
public:
    DX11();
    ~DX11();

    HRESULT Initialize();
    void CaptureAndAnalyze();

private:
    struct PixelLocation {
        int x;
        int y;
    };

    HRESULT AnalyzeScreenRegion();
    void CleanUp();
    double ColorDistance(BYTE r1, BYTE g1, BYTE b1, BYTE r2, BYTE g2, BYTE b2);
    HRESULT ReinitializeDesktopDuplication();

    ID3D11Device* device;
    ID3D11DeviceContext* context;
    IDXGIOutputDuplication* desktopDupl;
    ID3D11Texture2D* stagingTexture;
    D3D11_TEXTURE2D_DESC stagingDesc;

    int regionWidth;
    int regionHeight;
    int regionX;
    int regionY;

    std::chrono::time_point<std::chrono::high_resolution_clock> startTime;
    int frameCount;
    std::atomic<bool> shouldExit;

    IDXGIResource* desktopResource;
    ID3D11Texture2D* desktopTexture;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;

    std::vector<COLORREF> TARGET_COLORS;
    int tolerance;
    PixelLocation foundLocation;
};

double DX11::ColorDistance(BYTE r1, BYTE g1, BYTE b1, BYTE r2, BYTE g2, BYTE b2) {
    double rmean = (r1 + r2) / 2.0;
    int r = r1 - r2;
    int g = g1 - g2;
    int b = b1 - b2;
    double weightR = 2 + rmean / 256.0;
    double weightG = 4.0;
    double weightB = 2 + (255 - rmean) / 256.0;
    return std::sqrt(weightR * r * r + weightG * g * g + weightB * b * b);
}

DX11::DX11() :
    device(nullptr), context(nullptr), desktopDupl(nullptr), stagingTexture(nullptr),
    desktopResource(nullptr), desktopTexture(nullptr),
    regionWidth(40), regionHeight(40), frameCount(0), shouldExit(false) {
    regionX = GetSystemMetrics(SM_CXSCREEN) / 2;
    regionY = GetSystemMetrics(SM_CYSCREEN) / 2;
}

DX11::~DX11() {
    CleanUp();
}

HRESULT DX11::Initialize() {
    HRESULT hr;

    // Create D3D11 device
    D3D_FEATURE_LEVEL featureLevel;
    hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, &featureLevel, &context);
    if (FAILED(hr)) {
        std::cerr << "Failed to create D3D11 device. HRESULT: " << std::hex << hr << std::endl;
        return hr;
    }

    hr = ReinitializeDesktopDuplication();
    if (FAILED(hr)) {
        return hr;
    }

    // Create a staging texture for the region we want to analyze
    stagingDesc.Width = regionWidth;
    stagingDesc.Height = regionHeight;
    stagingDesc.MipLevels = 1;
    stagingDesc.ArraySize = 1;
    stagingDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.SampleDesc.Quality = 0;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    hr = device->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);
    if (FAILED(hr)) {
        std::cerr << "Failed to Create texture 2D " << std::endl;
        return hr;
    }

    return S_OK;
}

HRESULT DX11::ReinitializeDesktopDuplication() {
    HRESULT hr;

    if (desktopDupl) {
        desktopDupl->Release();
        desktopDupl = nullptr;
    }

    // Get DXGI device
    IDXGIDevice* dxgiDevice = nullptr;
    hr = device->QueryInterface(__uuidof(IDXGIDevice), reinterpret_cast<void**>(&dxgiDevice));
    if (FAILED(hr)) {
        std::cerr << "Failed to get DXGI device. HRESULT: " << std::hex << hr << std::endl;
        return hr;
    }

    // Get DXGI adapter
    IDXGIAdapter* dxgiAdapter = nullptr;
    hr = dxgiDevice->GetAdapter(&dxgiAdapter);
    dxgiDevice->Release();
    if (FAILED(hr)) {
        std::cerr << "Failed to get DXGI adapter. HRESULT: " << std::hex << hr << std::endl;
        return hr;
    }

    // Get primary output (monitor)
    IDXGIOutput* dxgiOutput = nullptr;
    hr = dxgiAdapter->EnumOutputs(0, &dxgiOutput);
    dxgiAdapter->Release();
    if (FAILED(hr)) {
        std::cerr << "Failed to get DXGI output. HRESULT: " << std::hex << hr << std::endl;
        return hr;
    }

    // QI for Output 1
    IDXGIOutput1* dxgiOutput1 = nullptr;
    hr = dxgiOutput->QueryInterface(__uuidof(IDXGIOutput1), reinterpret_cast<void**>(&dxgiOutput1));
    dxgiOutput->Release();
    if (FAILED(hr)) {
        std::cerr << "Failed to query IDXGIOutput1. HRESULT: " << std::hex << hr << std::endl;
        return hr;
    }

    // Create desktop duplication
    hr = dxgiOutput1->DuplicateOutput(device, &desktopDupl);
    dxgiOutput1->Release();
    if (FAILED(hr)) {
        std::cerr << "Failed to duplicate output. HRESULT: " << std::hex << hr << std::endl;
        return hr;
    }

    return S_OK;
}

void DX11::CaptureAndAnalyze() {
    startTime = std::chrono::high_resolution_clock::now();
    const UINT TIMEOUT_MS = 100; // 100ms timeout
    const int MAX_ATTEMPTS = 5;

    while (!shouldExit) {
        TARGET_COLORS = { RGB(234, 35, 1), RGB(218, 9, 1), RGB(227, 69, 53), RGB(227, 69, 53) };
        tolerance = 15;

        HRESULT hr = S_OK;
        int attempts = 0;

        do {
            if (desktopResource) {
                desktopResource->Release();
                desktopResource = nullptr;
            }

            hr = desktopDupl->AcquireNextFrame(TIMEOUT_MS, &frameInfo, &desktopResource);

            if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
                attempts++;
                Sleep(50);
                continue;
            }
            else if (FAILED(hr)) {
                if (hr == DXGI_ERROR_ACCESS_LOST) {
                    std::cerr << "Access lost. Attempting to reinitialize..." << std::endl;
                    hr = ReinitializeDesktopDuplication();
                    if (FAILED(hr)) {
                        std::cerr << "Failed to reinitialize desktop duplication. HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
                        return;
                    }
                    attempts = 0;
                    continue;
                }
                else {
                    std::cerr << "Failed to acquire frame. HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
                    attempts++;
                    Sleep(100);
                    continue;
                }
            }

            break; // Successfully acquired frame
        } while (attempts < MAX_ATTEMPTS);

        if (SUCCEEDED(hr)) {
            hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&desktopTexture));
            if (SUCCEEDED(hr)) {
                hr = AnalyzeScreenRegion();
                if (FAILED(hr)) {
                    std::cerr << "Failed to analyze screen region. HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
                }
                else if (foundLocation.x != -1 && foundLocation.y != -1) {
                    std::cout << "Found matching color at: ("
                        << foundLocation.x + (regionX - regionWidth / 2) << ", "
                        << foundLocation.y + (regionY - regionHeight / 2) << ")" << std::endl;
                }
                desktopTexture->Release();
                desktopTexture = nullptr;
            }
            else {
                std::cerr << "Failed to query desktop texture. HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
            }
            desktopDupl->ReleaseFrame();
        }
        else {
            std::cerr << "Failed to acquire frame after " << MAX_ATTEMPTS << " attempts." << std::endl;
        }

        frameCount++;

        // Calculate and print FPS every second
        auto currentTime = std::chrono::high_resolution_clock::now();
        std::chrono::duration<double> elapsedTime = currentTime - startTime;
        if (elapsedTime.count() >= 1.0) {
            double fps = frameCount / elapsedTime.count();
            std::cout << "FPS: " << fps << std::endl;

            startTime = std::chrono::high_resolution_clock::now();
            frameCount = 0;
        }
    }
}

HRESULT DX11::AnalyzeScreenRegion() {
    D3D11_TEXTURE2D_DESC desc;
    desktopTexture->GetDesc(&desc);

    UINT captureLeft = static_cast<UINT>(max(0, min(regionX - regionWidth / 2, static_cast<int>(desc.Width - regionWidth))));
    UINT captureTop = static_cast<UINT>(max(0, min(regionY - regionHeight / 2, static_cast<int>(desc.Height - regionHeight))));

    D3D11_BOX sourceRegion = {
        captureLeft,
        captureTop,
        0,
        captureLeft + regionWidth,
        captureTop + regionHeight,
        1
    };

    context->CopySubresourceRegion(stagingTexture, 0, 0, 0, 0, desktopTexture, 0, &sourceRegion);

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    HRESULT hr = context->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mappedResource);
    if (SUCCEEDED(hr)) {
        PBYTE dataPtr = static_cast<PBYTE>(mappedResource.pData);

        foundLocation = { -1, -1 };  // Reset found location

        for (int y = 0; y < regionHeight; ++y) {
            for (int x = 0; x < regionWidth; ++x) {
                int index = y * mappedResource.RowPitch + x * 4;  // 4 bytes per pixel (BGRA)
                BYTE blue = dataPtr[index];
                BYTE green = dataPtr[index + 1];
                BYTE red = dataPtr[index + 2];

                for (const auto& targetColor : TARGET_COLORS) {
                    if (ColorDistance(red, green, blue,
                        GetRValue(targetColor),
                        GetGValue(targetColor),
                        GetBValue(targetColor)) <= tolerance) {
                        foundLocation = { x, y };
                        context->Unmap(stagingTexture, 0);
                        return S_OK;  // Found a matching color
                    }
                }
            }
        }

        context->Unmap(stagingTexture, 0);
    }

    return hr;
}

void DX11::CleanUp() {
    if (desktopTexture) desktopTexture->Release();
    if (desktopResource) desktopResource->Release();
    if (stagingTexture) stagingTexture->Release();
    if (desktopDupl) desktopDupl->Release();
    if (context) context->Release();
    if (device) device->Release();
}

int main() {
    DX11 dx11;
    if (SUCCEEDED(dx11.Initialize())) {
        dx11.CaptureAndAnalyze();
    }
    return 0;
}