#include <windows.h>
#include <dwmapi.h>
#include <d3d11.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <unordered_set>
#include <Pdh.h>
#include <vector>
#include <unordered_map>

// Dear ImGui headers (Assuming you downloaded them into your project)
#include "./imgui-resources/imgui.h"
#include "./imgui-resources/imgui_impl_win32.h"
#include "./imgui-resources/imgui_impl_dx11.h"


// Link the required Windows libraries
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "pdh.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// 1. Create a borderless, layered, click-through window
LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
        case WM_HOTKEY:
            if(wParam == 1){
                PostQuitMessage(0);
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// ====================================================================
//             Helper functions for RAM tracker
// ====================================================================
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

// 1. New Shared Helper: Gathers all PIDs in the tree
std::unordered_set<DWORD> GetProcessTreePids(DWORD rootPid) {
    std::unordered_set<DWORD> treePids;
    if (rootPid == 0) return treePids;
    
    treePids.insert(rootPid);
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return treePids;

    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);

    bool addedNew = true;
    while (addedNew) {
        addedNew = false;
        if (Process32First(hSnapshot, &pe32)) {
            do {
                if (treePids.count(pe32.th32ParentProcessID) > 0 && treePids.count(pe32.th32ProcessID) == 0) {
                    treePids.insert(pe32.th32ProcessID);
                    addedNew = true;
                }
            } while (Process32Next(hSnapshot, &pe32));
        }
    }
    CloseHandle(hSnapshot);
    return treePids;
}

// 2. Updated RAM Calculator: Now accepts the shared tree
size_t GetProcessTreeRAM(const std::unordered_set<DWORD>& treePids) {
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
    return totalRamBytes / (1024 * 1024);
}

// ====================================================================
//             Helper functions for CPU tracker
// ====================================================================
struct CpuUsageTracker {
    DWORD lastRootPid = 0;
    ULARGE_INTEGER lastSystemTime = { 0 };
    ULARGE_INTEGER lastProcessTreeTime = { 0 };
    int numProcessors = 0;

    CpuUsageTracker() {
        SYSTEM_INFO sysInfo;
        GetSystemInfo(&sysInfo);
        numProcessors = sysInfo.dwNumberOfProcessors;
    }

    double GetUsage(DWORD currentRootPid, const std::unordered_set<DWORD>& treePids) {
        if (currentRootPid == 0 || treePids.empty()) return 0.0;

        FILETIME sysTimeNow;
        GetSystemTimeAsFileTime(&sysTimeNow);
        ULARGE_INTEGER sysCurrent;
        sysCurrent.LowPart = sysTimeNow.dwLowDateTime;
        sysCurrent.HighPart = sysTimeNow.dwHighDateTime;

        // Sum the kernel and user time for EVERY process in the tree
        ULARGE_INTEGER totalProcCurrent = { 0 };
        for (DWORD pid : treePids) {
            HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hProcess) {
                FILETIME creationTime, exitTime, kernelTime, userTime;
                if (GetProcessTimes(hProcess, &creationTime, &exitTime, &kernelTime, &userTime)) {
                    ULARGE_INTEGER kTime, uTime;
                    kTime.LowPart = kernelTime.dwLowDateTime;
                    kTime.HighPart = kernelTime.dwHighDateTime;
                    uTime.LowPart = userTime.dwLowDateTime;
                    uTime.HighPart = userTime.dwHighDateTime;
                    totalProcCurrent.QuadPart += (kTime.QuadPart + uTime.QuadPart);
                }
                CloseHandle(hProcess);
            }
        }

        double cpuUsage = 0.0;

        if (lastRootPid != currentRootPid) {
            lastRootPid = currentRootPid;
            lastSystemTime = sysCurrent;
            lastProcessTreeTime = totalProcCurrent;
            return 0.0; 
        }

        ULONGLONG sysDelta = sysCurrent.QuadPart - lastSystemTime.QuadPart;
        
        // Prevent negative spikes if a heavy child process closed between ticks
        if (totalProcCurrent.QuadPart >= lastProcessTreeTime.QuadPart) {
            ULONGLONG procDelta = totalProcCurrent.QuadPart - lastProcessTreeTime.QuadPart;
            if (sysDelta > 0) {
                cpuUsage = ((double)procDelta / (double)sysDelta) * 100.0 / numProcessors;
            }
        }

        lastSystemTime = sysCurrent;
        lastProcessTreeTime = totalProcCurrent;

        return cpuUsage;
    }
};

// ====================================================================
//             Helper functions for hdd tracker
// ====================================================================
struct DiskIoTracker {
    DWORD lastRootPid = 0;
    ULONGLONG lastTime = 0;
    ULONGLONG lastReadBytes = 0;
    ULONGLONG lastWriteBytes = 0;
    double readSpeedMBps = 0.0;
    double writeSpeedMBps = 0.0;

    void Update(DWORD currentRootPid, const std::unordered_set<DWORD>& treePids, ULONGLONG currentTime) {
        if (currentRootPid == 0 || treePids.empty()) {
            readSpeedMBps = 0.0;
            writeSpeedMBps = 0.0;
            return;
        }

        // Sum read and write bytes across the entire process tree
        ULONGLONG totalRead = 0;
        ULONGLONG totalWrite = 0;
        for (DWORD pid : treePids) {
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (hProc) {
                IO_COUNTERS ioCounters;
                if (GetProcessIoCounters(hProc, &ioCounters)) {
                    totalRead += ioCounters.ReadTransferCount;
                    totalWrite += ioCounters.WriteTransferCount;
                }
                CloseHandle(hProc);
            }
        }

        // If the user switched apps, reset baseline
        if (lastRootPid != currentRootPid || lastTime == 0) {
            lastRootPid = currentRootPid;
            lastTime = currentTime;
            lastReadBytes = totalRead;
            lastWriteBytes = totalWrite;
            readSpeedMBps = 0.0;
            writeSpeedMBps = 0.0;
            return;
        }

        double elapsedSeconds = (double)(currentTime - lastTime) / 1000.0;
        if (elapsedSeconds > 0.0) {
            ULONGLONG readDelta = (totalRead >= lastReadBytes) ? (totalRead - lastReadBytes) : 0;
            ULONGLONG writeDelta = (totalWrite >= lastWriteBytes) ? (totalWrite - lastWriteBytes) : 0;

            readSpeedMBps = ((double)readDelta / elapsedSeconds) / (1024.0 * 1024.0);
            writeSpeedMBps = ((double)writeDelta / elapsedSeconds) / (1024.0 * 1024.0);
        }

        lastTime = currentTime;
        lastReadBytes = totalRead;
        lastWriteBytes = totalWrite;
    }
};

// ====================================================================
//             Helper functions for GPU tracker
// ====================================================================
struct GpuUsageTracker {
    PDH_HQUERY hQuery = nullptr;
    std::unordered_map<DWORD, PDH_HCOUNTER> pidCounters;

    ~GpuUsageTracker() {
        if (hQuery) PdhCloseQuery(hQuery);
    }

    double GetUsage(const std::unordered_set<DWORD>& treePids) {
        if (treePids.empty()) return 0.0;

        if (!hQuery) PdhOpenQuery(nullptr, 0, &hQuery);

        bool queryChanged = false;

        // 1. Remove counters for child processes that have closed
        for (auto it = pidCounters.begin(); it != pidCounters.end(); ) {
            if (treePids.count(it->first) == 0) {
                PdhRemoveCounter(it->second);
                it = pidCounters.erase(it);
                queryChanged = true;
            } else {
                ++it;
            }
        }

        // 2. Add new counters for spawned child processes
        for (DWORD pid : treePids) {
            if (pidCounters.count(pid) == 0) {
                PDH_HCOUNTER hCounter;
                char counterPath[256];
                // The * wildcard here fetches all engines (3D, Copy, Video) for this specific PID
                sprintf_s(counterPath, "\\GPU Engine(pid_%lu_*)\\Utilization Percentage", pid);
                if (PdhAddEnglishCounterA(hQuery, counterPath, 0, &hCounter) == ERROR_SUCCESS) {
                    pidCounters[pid] = hCounter;
                    queryChanged = true;
                }
            }
        }

        if (pidCounters.empty()) return 0.0;

        // 3. If the tree changed, we must collect once to establish a baseline and skip this frame
        if (queryChanged) {
            PdhCollectQueryData(hQuery);
            return 0.0; 
        }

        PdhCollectQueryData(hQuery);

        // 4. Sum the GPU utilization across the entire process tree
        double totalGpu = 0.0;
        for (auto const& pair : pidCounters) {
            PDH_HCOUNTER hCounter = pair.second;
            DWORD bufferSize = 0;
            DWORD itemCount = 0;
            PdhGetFormattedCounterArrayA(hCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, nullptr);

            if (bufferSize > 0 && itemCount > 0) {
                std::vector<char> buffer(bufferSize);
                auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_A*>(buffer.data());
                if (PdhGetFormattedCounterArrayA(hCounter, PDH_FMT_DOUBLE, &bufferSize, &itemCount, items) == ERROR_SUCCESS) {
                    for (DWORD i = 0; i < itemCount; i++) {
                        totalGpu += items[i].FmtValue.doubleValue;
                    }
                }
            }
        }

        return totalGpu;
    }
};


// ====================================================================
//             Main Function
// ====================================================================
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

    // Register Ctrl + Shift + Q to close the overlay (ID = 1)
    RegisterHotKey(hwnd, 1, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'Q');

    // 6. Main Application Loop
    MSG msg;
    bool done = false;

    // Polling Throttling Variables
    static ULONGLONG lastUpdateTime = 0;
    static DWORD cachedPid = 0;
    static size_t cachedRamMB = 0;
    static char cachedProcessName[MAX_PATH] = "Unknown";
    static double cachedCpuUsage = 0.0; // Store CPU percentage
    static double cachedGpuUsage = 0.0; //Store GPU percentage
    
    CpuUsageTracker cpuTracker; // Initialize the tracker
    DiskIoTracker diskTracker;
    GpuUsageTracker gpuTracker;


    while (!done) {
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // ====================================================
        // THROTTLED TELEMETRY GATHERING (Runs once per second)
        // ====================================================
        ULONGLONG currentTime = GetTickCount64();
        if (currentTime - lastUpdateTime >= 1000) { 
            HWND hForeground = GetForegroundWindow();
            GetWindowThreadProcessId(hForeground, &cachedPid);
            
            // 1. Fetch Process Name
            HANDLE hProcessInfo = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, cachedPid);
            bool nameFound = false;
            if (hProcessInfo) {
                char exePath[MAX_PATH];
                DWORD size = MAX_PATH;
                if (QueryFullProcessImageNameA(hProcessInfo, 0, exePath, &size)) {
                    char* fileName = strrchr(exePath, '\\');
                    if (fileName) strcpy_s(cachedProcessName, sizeof(cachedProcessName), fileName + 1);
                    else strcpy_s(cachedProcessName, sizeof(cachedProcessName), exePath);
                    nameFound = true;
                }
                CloseHandle(hProcessInfo);
            }
            if (!nameFound) sprintf_s(cachedProcessName, sizeof(cachedProcessName), "PID: %lu", cachedPid);

            //get the shared process tree
            std::unordered_set<DWORD> treePids = GetProcessTreePids(cachedPid);

            // 2. Fetch Tree RAM
            cachedRamMB = GetProcessTreeRAM(cachedPid);
            
            // 3. Fetch CPU Usage
            cachedCpuUsage = cpuTracker.GetUsage(cachedPid, treePids);

            //4. Fetch HDD Usage
            diskTracker.Update(cachedPid, treePids, currentTime);

            //5. Fetch GPU Usage
            cachedGpuUsage = gpuTracker.GetUsage(treePids);
            
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
        ImGui::SetNextWindowBgAlpha(1.0f);
        
        ImGui::Begin("Dev Stats", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings);

        ImGui::Text("Active: %s", cachedProcessName);
        ImGui::Separator();
        
        //Print CPU Usage Colored Text
        ImVec4 cpuColor = (cachedCpuUsage > 80.0f) ? ImVec4(1.0f, 0.0f, 0.0f, 1.0f) : // Red
                          (cachedCpuUsage > 30.0f) ? ImVec4(1.0f, 1.0f, 0.0f, 1.0f) : // Yellow
                                                     ImVec4(0.0f, 1.0f, 0.0f, 1.0f);  // Green
        ImGui::TextColored(cpuColor, "CPU: %.1f%%", cachedCpuUsage);
        
        //Print GPU Usage Colored Text
        ImVec4 gpuColor = (cachedGpuUsage > 80.0f) ? ImVec4(1.0f, 0.0f, 0.0f, 1.0f) :
                          (cachedGpuUsage > 30.0f) ? ImVec4(1.0f, 1.0f, 0.0f, 1.0f) : ImVec4(0.0f, 1.0f, 0.0f, 1.0f);
        ImGui::TextColored(gpuColor, "GPU: %.1f%%", cachedGpuUsage);

        //Print RAM Usage Colored Text
        ImGui::Text("RAM: %zu MB", cachedRamMB);
        
        //Print Disk Usage Colored Text
        ImVec4 diskColor = (diskTracker.readSpeedMBps > 10.0f) ? ImVec4(0.0f, 0.8f, 1.0f, 1.0f) : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        ImGui::TextColored(diskColor, "Disk Read: %.2f MB/s", diskTracker.readSpeedMBps);
        ImGui::Text("Disk Write: %.2f MB/s", diskTracker.writeSpeedMBps);

        ImGui::End();

        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    UnregisterHotKey(hwnd, 1); // NEW: Release the hotkey
    
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}
