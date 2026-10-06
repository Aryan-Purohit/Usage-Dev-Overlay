#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <unordered_set>

// Dear ImGui headers (Assuming you downloaded them into your project)
#include "./imgui-resources/imgui.h"
#include "./imgui-resources/imgui_impl_win32.h"
#include "./imgui-resources/imgui_impl_dx11.h"


// Link the required Windows libraries
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "psapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// 1. Create a borderless, layered, click-through window
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

//HElper function for process root tree calculation
size_t GetProcessTreeRAM(DWORD rootPid)
{
    if(rootPid == 0) return 0;

    std::unordered_set<DWORD> treePids;
    treePids.insert(rootPid);

    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);

// Traverse the snapshot repeatedly to find all deep children (e.g., grandchildren)
    bool addedNew = true;
    while (addedNew) {
        addedNew = false;
        if (Process32First(hSnapshot, &pe32)) {
            do {
        // If we found a child of a PID we are already tracking, add it to our set
                if (treePids.count(pe32.th32ParentProcessID) > 0 && treePids.count(pe32.th32ProcessID) == 0) {
                    treePids.insert(pe32.th32ProcessID);
                    addedNew = true;
                }
            } while (Process32Next(hSnapshot, &pe32));
        }
    }
CloseHandle(hSnapshot);

// Sum the Working Set memory for every PID in the tree
    size_t totalRamBytes = 0;
    for (DWORD pid : treePids) {
        HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
        if (hProc) {
            PROCESS_MEMORY_COUNTERS pmc;
            if (GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) {
                totalRamBytes += pmc.WorkingSetSize;
            }
            CloseHandle(hProc);
        }
    }

    return totalRamBytes / (1024 * 1024); // Convert to MB
}


int APIENTRY wWinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPWSTR lpCmdLine, _In_ int nCmdShow) {
    
    // 1. Register the Window Class
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, nullptr, nullptr, nullptr, nullptr, L"MyOverlayClass", nullptr };
    RegisterClassExW(&wc);

    // 2. Create the window
    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"DirectX Overlay",
        WS_POPUP,
        0, 0, 1920, 1080,
        nullptr, nullptr, wc.hInstance, nullptr
    );

SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 255, LWA_ALPHA);
    MARGINS margins = { -1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // 4. Initialize DirectX 11
    ID3D11Device* g_pd3dDevice = nullptr;
    ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
    IDXGISwapChain* g_pSwapChain = nullptr;
    ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, 
        nullptr, 0, D3D11_SDK_VERSION, &sd, 
        &g_pSwapChain, &g_pd3dDevice, nullptr, &g_pd3dDeviceContext
    );

    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();

    // 5. Initialize ImGui
    ImGui::CreateContext();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    const float clear_color_with_alpha[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

// 6. Main Application Loop
    MSG msg;
    bool done = false;

    // Polling Throttling Variables
    static ULONGLONG lastUpdateTime = 0;
    static DWORD cachedPid = 0;
    static size_t cachedRamMB = 0;
    static char cachedProcessName[MAX_PATH] = "Unknown"; // Stores the display name

    while (!done) {
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // ==========================================
        // THROTTLED TELEMETRY GATHERING (Runs once per second)
        // ==========================================
        ULONGLONG currentTime = GetTickCount64();
        if (currentTime - lastUpdateTime >= 1000) { 
            HWND hForeground = GetForegroundWindow();
            GetWindowThreadProcessId(hForeground, &cachedPid);
            
            // 1. Fetch Process Name (with fallback to PID)
            HANDLE hProcessInfo = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, cachedPid);
            bool nameFound = false;
            if (hProcessInfo) {
                char exePath[MAX_PATH];
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameA(hProcessInfo, 0, exePath, &size)) {
                    // Find the last backslash in the full path to isolate the file name
                    char* fileName = strrchr(exePath, '\\');
                    if (fileName) {
                        strcpy_s(cachedProcessName, sizeof(cachedProcessName), fileName + 1);
                    } else {
                        strcpy_s(cachedProcessName, sizeof(cachedProcessName), exePath);
                    }
                    nameFound = true;
                }
                CloseHandle(hProcessInfo);
            }
            
            // Fallback if Windows denies access to the process name
            if (!nameFound) {
                sprintf_s(cachedProcessName, sizeof(cachedProcessName), "PID: %lu", cachedPid);
            }

            // 2. Calculate Tree RAM
            cachedRamMB = GetProcessTreeRAM(cachedPid);
            
            // 3. Calculate IO/HDD Stats
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, cachedPid);
            if (hProcess) {
                IO_COUNTERS ioCounters;
                if (GetProcessIoCounters(hProcess, &ioCounters)) {
                    ULONGLONG totalBytesRead = ioCounters.ReadTransferCount;
                    ULONGLONG totalBytesWritten = ioCounters.WriteTransferCount;
                }
                CloseHandle(hProcess);
            }

            lastUpdateTime = currentTime;
        }

        // ==========================================
        // UI RENDERING (Runs 60+ FPS)
        // ==========================================
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.65f);
        ImGui::Begin("Dev Stats", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);

        // Display the dynamically formatted string
        ImGui::Text("Active: %s", cachedProcessName);
        ImGui::Separator();
        ImGui::Text("Tree RAM Usage: %zu MB", cachedRamMB);

        ImGui::End();

        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }
}
