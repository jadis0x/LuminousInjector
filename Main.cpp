﻿#include <Windows.h>
#include <iostream>
#include <TlHelp32.h>
#include <shobjidl.h>

#include "nlohmann/json.hpp"
#include <fstream>

const wchar_t* getWCharPtr(const std::wstring& str) {
    return str.c_str();
}

DWORD GetProcessIdByName(const wchar_t* processName) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    PROCESSENTRY32 pe;
    pe.dwSize = sizeof(PROCESSENTRY32);

    if (!Process32First(hSnapshot, &pe)) {
        CloseHandle(hSnapshot);
        return 0;
    }

    do {
        if (_wcsicmp(pe.szExeFile, processName) == 0) {
            CloseHandle(hSnapshot);
            return pe.th32ProcessID;
        }
    } while (Process32Next(hSnapshot, &pe));

    CloseHandle(hSnapshot);
    return 0;
}

// Function to inject the DLL into the target process
bool InjectDLL(DWORD processId, const char* dllPath) {
    // Open the target process
    HANDLE hProcess = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processId);
    if (hProcess == NULL) {
        std::cerr << "Failed to open process " << processId << std::endl;
        return false;
    }

    // Allocate memory in the target process for the path to the DLL
    LPVOID dllPathAddr = VirtualAllocEx(hProcess, NULL, strlen(dllPath) + 1, MEM_COMMIT, PAGE_READWRITE);
    if (dllPathAddr == NULL) {
        std::cerr << "Failed to allocate memory in process " << processId << std::endl;
        CloseHandle(hProcess);
        return false;
    }

    // Write the DLL path to the allocated memory
    if (!WriteProcessMemory(hProcess, dllPathAddr, dllPath, strlen(dllPath) + 1, NULL)) {
        std::cerr << "Failed to write to process " << processId << std::endl;
        VirtualFreeEx(hProcess, dllPathAddr, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    // Get the address of the LoadLibrary function in kernel32.dll
    LPVOID loadLibraryAddr = (LPVOID)GetProcAddress(GetModuleHandle(TEXT("kernel32.dll")), "LoadLibraryA");
    if (loadLibraryAddr == NULL) {
        std::cerr << "Failed to get address of LoadLibraryA" << std::endl;
        VirtualFreeEx(hProcess, dllPathAddr, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    HANDLE hRemoteThread = CreateRemoteThread(hProcess, NULL, 0, (LPTHREAD_START_ROUTINE)loadLibraryAddr, dllPathAddr, 0, NULL);
    if (hRemoteThread == NULL) {
        std::cerr << "Failed to create remote thread in process " << processId << std::endl;
        VirtualFreeEx(hProcess, dllPathAddr, 0, MEM_RELEASE);
        CloseHandle(hProcess);
        return false;
    }

    WaitForSingleObject(hRemoteThread, INFINITE);

    VirtualFreeEx(hProcess, dllPathAddr, 0, MEM_RELEASE);
    CloseHandle(hRemoteThread);
    CloseHandle(hProcess);

    return true;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED |
        COINIT_DISABLE_OLE1DDE);

    if (FAILED(hr)) {
        MessageBox(NULL, L"CoInitializeEx function failed.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    std::ifstream configFile("config.json");
    if (!configFile.is_open()) {
        MessageBox(NULL, L"The config file could not be opened!", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    nlohmann::json configJson;
    configFile >> configJson;

    std::string processNameStr = configJson["processName"];
    std::wstring processNameWStr(processNameStr.begin(), processNameStr.end());
    const wchar_t* processName = processNameWStr.c_str();

    IFileOpenDialog* pFileOpen;
    hr = CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_ALL,
        IID_IFileOpenDialog, reinterpret_cast<void**>(&pFileOpen));

    if (FAILED(hr)) {
        MessageBox(NULL, L"Failed to open file open dialog box.", L"Error", MB_OK | MB_ICONERROR);
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
        MessageBox(NULL, L"Failed to open file open dialog box.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    IShellItem* pItem;
    hr = pFileOpen->GetResult(&pItem);

    if (FAILED(hr)) {
        MessageBox(NULL, L"Failed to retrieve file name.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    PWSTR pszFilePath;
    hr = pItem->GetDisplayName(SIGDN_FILESYSPATH, &pszFilePath);

    if (FAILED(hr)) {
        MessageBox(NULL, L"Failed to retrieve file name.", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    const wchar_t* dllPath = pszFilePath;

    DWORD targetProcessId = GetProcessIdByName(processName);
    if (targetProcessId == 0) {
        int choice = MessageBox(NULL, L"The target process is not running. Do you want to launch it?", L"Process Not Running", MB_YESNO | MB_ICONQUESTION);
        if (choice == IDYES) {
            HINSTANCE result = ShellExecute(NULL, L"open", dllPath, NULL, NULL, SW_SHOWNORMAL);
            if ((int)result <= 32) {
                MessageBox(NULL, L"Failed to launch the target process", L"Error", MB_OK | MB_ICONERROR);
                return 1;
            }
        }
        else {
            MessageBox(NULL, L"The target process will not be launched.", L"Info", MB_OK | MB_ICONINFORMATION);
            return 1;
        }
    }

    std::wstring tmp(dllPath);
    std::string dll_path_str(tmp.begin(), tmp.end());

    if (InjectDLL(targetProcessId, dll_path_str.c_str())) {

    }
    else {
        MessageBox(NULL, L"Failed to inject DLL into process", L"LuminousInjector", MB_OK | MB_ICONERROR);
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