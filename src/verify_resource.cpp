// Tool to verify if the ASI has the correct resources
#include <windows.h>
#include <iostream>
#include <vector>


BOOL CALLBACK EnumNamesFunc(HMODULE hModule, LPCWSTR lpType, LPWSTR lpName, LONG_PTR lParam) {
    std::wcout << L"    Name: ";
    if (IS_INTRESOURCE(lpName)) {
        std::wcout << (int)(size_t)lpName;
    } else {
        std::wcout << lpName;
    }
    std::wcout << std::endl;
    return TRUE;
}

BOOL CALLBACK EnumTypesFunc(HMODULE hModule, LPWSTR lpType, LONG_PTR lParam) {
    std::wcout << L"Type: ";
    if (IS_INTRESOURCE(lpType)) {
        std::wcout << (int)(size_t)lpType;
    } else {
        std::wcout << lpType;
    }
    std::wcout << std::endl;
    
    EnumResourceNamesW(hModule, lpType, EnumNamesFunc, 0);
    return TRUE;
}


// Helper to check PE header
void CheckPEHeader(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        std::cout << "Failed to open file for PE check." << std::endl;
        return;
    }

    IMAGE_DOS_HEADER dosHeader;
    fread(&dosHeader, sizeof(dosHeader), 1, f);
    
    if (dosHeader.e_magic != IMAGE_DOS_SIGNATURE) {
        std::cout << "Invalid DOS signature." << std::endl;
        fclose(f);
        return;
    }

    fseek(f, dosHeader.e_lfanew, SEEK_SET);
    
    DWORD peSignature;
    fread(&peSignature, sizeof(peSignature), 1, f);
    
    if (peSignature != IMAGE_NT_SIGNATURE) {
        std::cout << "Invalid PE signature." << std::endl;
        fclose(f);
        return;
    }

    IMAGE_FILE_HEADER fileHeader;
    fread(&fileHeader, sizeof(fileHeader), 1, f);

    std::cout << "Machine: 0x" << std::hex << fileHeader.Machine << std::dec << " ";
    if (fileHeader.Machine == IMAGE_FILE_MACHINE_AMD64) {
        std::cout << "(x64 - CORRECT)" << std::endl;
    } else if (fileHeader.Machine == IMAGE_FILE_MACHINE_I386) {
        std::cout << "(x86 - WRONG)" << std::endl;
    } else {
        std::cout << "(Unknown)" << std::endl;
    }

    fclose(f);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: VerifyResource <path_to_asi>" << std::endl;
        return 1;
    }

    CheckPEHeader(argv[1]);

    std::cout << "Attempting LOAD_LIBRARY_AS_DATAFILE..." << std::endl;
    HMODULE hModData = LoadLibraryExA(argv[1], NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (!hModData) {
        std::cout << "Failed to load library as data: " << GetLastError() << std::endl;
        return 1;
    }

    std::cout << "Enumerating resources in: " << argv[1] << std::endl;
    EnumResourceTypesW(hModData, EnumTypesFunc, 0);
    FreeLibrary(hModData);

    std::cout << "\nAttempting Full LoadLibrary (Exec)..." << std::endl;
    HMODULE hModExec = LoadLibraryA(argv[1]);
    if (!hModExec) {
        DWORD err = GetLastError();
        std::cout << "CRITICAL: Failed to load library: " << err << std::endl;
        
        char msgUtils[1024];
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                      NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                      msgUtils, sizeof(msgUtils), NULL);
        std::cout << "Error Message: " << msgUtils << std::endl;
        
        if (err == 126) std::cout << "Hint: Missing dependency DLL?" << std::endl;
        if (err == 193) std::cout << "Hint: Architecture mismatch (32/64 bit)?" << std::endl;
    } else {
        std::cout << "SUCCESS: Library loaded successfully into test process!" << std::endl;
        std::cout << "This confirms the DLL file is valid and dependencies are met." << std::endl;
        FreeLibrary(hModExec);
    }

    return 0;
}
