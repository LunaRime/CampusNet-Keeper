#include <cstdio>
#include "core/Version.h"

int wmain(int, wchar_t**) {
    std::printf("CampusNet-Keeper %s\n", cnk::VersionString());
    return 0;
}
