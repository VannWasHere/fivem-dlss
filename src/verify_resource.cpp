// Tool to verify if the ASI has the correct resources
#include <windows.h>
#include <iostream>
#include <vector>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: VerifyResource <path_to_asi>" << std::endl;
        return 1;
    }

    // Load the ASI as a data file first to check existence
    HMODULE hMod = LoadLibraryExA(argv[1], NULL, LOAD_LIBRARY_AS_DATAFILE);
    if (!hMod) {
        std::cout << "Failed to load library: " << GetLastError() << std::endl;
        return 1;
    }

    std::cout << "Loaded library: " << argv[1] << std::endl;

    // Check for build 3258
    // Name: "FX_ASI_BUILD", Type: 3258
    HRSRC hRes = FindResourceW(hMod, L"FX_ASI_BUILD", MAKEINTRESOURCEW(3258));
    if (hRes) {
        std::cout << "SUCCESS: Found resource Name='FX_ASI_BUILD' Type=3258" << std::endl;
    } else {
        std::cout << "FAILED: Could not find Name='FX_ASI_BUILD' Type=3258. Error: " << GetLastError() << std::endl;
    }

    // Check converse: Name=3258, Type="FX_ASI_BUILD"
    hRes = FindResourceW(hMod, MAKEINTRESOURCEW(3258), L"FX_ASI_BUILD");
    if (hRes) {
        std::cout << "FOUND ALTERNATIVE: Name=3258 Type='FX_ASI_BUILD'" << std::endl;
    }

    FreeLibrary(hMod);
    return 0;
}
