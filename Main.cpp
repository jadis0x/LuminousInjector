#include <Windows.h>
#include <iostream>
#include <TlHelp32.h>
#include <shobjidl.h>

#include "nlohmann/json.hpp"
#include <fstream>
#include <string>

DWORD GetProcessIdByName(const wchar_t* processName) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32W pe;
    pe.dwSize = sizeof(PROCESSENTRY32W);

    if (!Process32FirstW(hSnapshot, &pe)) {
        CloseHandle(hSnapshot);
        return 0;
    }

    do {
        if (_wcsicmp(pe.szExeFile, processName) == 0) {
            CloseHandle(hSnapshot);
            return pe.th32ProcessID;
        }
    } while (Process32NextW(hSnapshot, &pe));

    CloseHandle(hSnapshot);
    return 0;
}

// Function to inject the DLL into the target process
bool InjectDLL(DWORD processId, const wchar_t* dllPath) {
    // Open the target process
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId);
    if (hProcess == NULL) {
        std::cerr << "Failed to open process " << processId << std::endl;
        return false;
    }

    // Allocate memory in the target process for the path to the DLL
    SIZE_T dllPathSize = (wcslen(dllPath) + 1) * sizeof(wchar_t);
    LPVOID dllPathAddr = VirtualAllocEx(hProcess, NULL, dllPathSize, MEM_COMMIT, PAGE_READWRITE);
    if (dllPathAddr == NULL) {
        std::cerr << "Failed to allocate memory in process " << processId << std::endl;
        CloseHandle(hProcess);
        return false;
    }

    // Write the DLL path to the allocated memory
    if (!WriteProcessMemory(hProcess, dllPathAddr, dllPath, dllPathSize, NULL)) {
        std::cerr << "Failed to write to process " << processId << std::endl;
        VirtualFreeEx(hProcess, dllPathAddr, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // Get the address of the LoadLibraryW function in kernel32.dll
    LPVOID loadLibraryAddr = (LPVOID)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    if (loadLibraryAddr == NULL) {
        std::cerr << "Failed to get address of LoadLibraryW" << std::endl;
        VirtualFreeEx(hProcess, dllPathAddr, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // Create a remote thread that calls LoadLibraryW with the DLL path
    HANDLE hRemoteThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)loadLibraryAddr, dllPathAddr, 0, NULL);
    if (hRemoteThread == NULL) {
        std::cerr << "Failed to create remote thread in process " << processId << std::endl;
        VirtualFreeEx(hProcess, dllPathAddr, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // Wait for the remote thread to finish
    WaitForSingleObject(hRemoteThread, INFINITE);

    // Clean up
    VirtualFreeEx(hProcess, dllPathAddr, 0, MEM_RELEASE);
    CloseHandle(hRemoteThread);
    CloseHandle(hProcess);

    return true;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED |
        COINIT_DISABLE_OLE1DDE);

    if (FAILED(hr)) {
        MessageBoxW(NULL, L"CoInitializeEx function failed.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    std::ifstream configFile("config.json");
    if (!configFile.is_open()) {
        MessageBoxW(NULL, L"The config file could not be opened!", L"Error", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    nlohmann::json configJson;
    configFile >> configJson;

    std::string processNameStr = configJson["processName"];
    std::wstring processNameWStr(processNameStr.begin(), processNameStr.end());
    const wchar_t* processName = processNameWStr.c_str();

    IFileOpenDialog* pFileOpen;
    hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
        IID_PPV_ARGS(&pFileOpen));

    if (FAILED(hr)) {
        MessageBoxW(NULL, L"Failed to open file open dialog box.", L"Error", MB_OK | MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    COMDLG_FILTERSPEC rgSpec[] =
    {
        { L"Dynamic Link Libraries (*.dll)", L"*.dll" }
    };

    pFileOpen->SetDefaultExtension(L"dll");
    pFileOpen->SetTitle(L"Select the .dll file to inject.");
    pFileOpen->SetFileTypes(ARRAYSIZE(rgSpec), rgSpec);
    hr = pFileOpen->Show(NULL);

    if (FAILED(hr)) {
        MessageBoxW(NULL, L"Failed to open file open dialog box.", L"Error", MB_OK | MB_ICONERROR);
        pFileOpen->Release();
        CoUninitialize();
        return 1;
    }

    IShellItem* pItem;
    hr = pFileOpen->GetResult(&pItem);

    if (FAILED(hr)) {
        MessageBoxW(NULL, L"Failed to retrieve file name.", L"Error", MB_OK | MB_ICONERROR);
        pFileOpen->Release();
        CoUninitialize();
        return 1;
    }

    PWSTR pszFilePath;
    hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);

    if (FAILED(hr)) {
        MessageBoxW(NULL, L"Failed to retrieve file name.", L"Error", MB_OK | MB_ICONERROR);
        pItem->Release();
        pFileOpen->Release();
        CoUninitialize();
        return 1;
    }

    const wchar_t* dllPath = pszFilePath;

    DWORD targetProcessId = GetProcessIdByName(processName);
    if (targetProcessId == 0) {
        int choice = MessageBoxW(NULL, L"The target process is not running. Do you want to launch it?", L"Process Not Running", MB_YESNO | MB_ICONQUESTION);
        if (choice == IDYES) {
            // Launch the target process
            SHELLEXECUTEINFOW sei = { 0 };
            sei.cbSize = sizeof(SHELLEXECUTEINFOW);
            sei.fMask = SEE_MASK_NOCLOSEPROCESS;
            sei.lpFile = processName;
            sei.nShow = SW_SHOWNORMAL;

            if (!ShellExecuteExW(&sei)) {
                MessageBoxW(NULL, L"Failed to launch the target process", L"Error", MB_OK | MB_ICONERROR);
                CoTaskMemFree(pszFilePath);
                pItem->Release();
                pFileOpen->Release();
                CoUninitialize();
                return 1;
            }

            // Wait for the process to initialize
            Sleep(2000);

            targetProcessId = GetProcessIdByName(processName);
            if (targetProcessId == 0) {
                MessageBoxW(NULL, L"Failed to find the target process after launching.", L"Error", MB_OK | MB_ICONERROR);
                CoTaskMemFree(pszFilePath);
                pItem->Release();
                pFileOpen->Release();
                CoUninitialize();
                return 1;
            }
        }
        else {
            MessageBoxW(NULL, L"The target process will not be launched.", L"Info", MB_OK | MB_ICONINFORMATION);
            CoTaskMemFree(pszFilePath);
            pItem->Release();
            pFileOpen->Release();
            CoUninitialize();
            return 1;
        }
    }

    if (InjectDLL(targetProcessId, dllPath)) {
        //MessageBoxW(NULL, L"Successfully injected the DLL into the target process.", L"Success", MB_OK | MB_ICONINFORMATION);
    }
    else {
        MessageBoxW(NULL, L"Failed to inject DLL into process.", L"Error", MB_OK | MB_ICONERROR);
        CoTaskMemFree(pszFilePath);
        pItem->Release();
        pFileOpen->Release();
        CoUninitialize();
        return 1;
    }

    CoTaskMemFree(pszFilePath);
    pItem->Release();
    pFileOpen->Release();
    CoUninitialize();
    return 0;
}
