#include <string>

#include "doctest.h"
#include "core/Version.h"

TEST_CASE("VersionString 返回非空版本号") {
    CHECK(cnk::VersionString() != nullptr);
    CHECK(std::string(cnk::VersionString()).size() > 0);
}
