#include <gtest/gtest.h>
#include "dependency_parser.hpp"

TEST(DependencyParser, SeparatesBatchTargetsAndDecodesMakeEscapes) {
    const auto deps = parse_makefile(
        "obj\\ one.o: src\\ one.c include/a.h \\\n"
        " include/space\\ header.h include/dollar$$.h\n"
        "other.o: other.c include/a.h\n");
    ASSERT_EQ(deps.size(), 2);
    ASSERT_TRUE(deps.contains("obj one.o"));
    EXPECT_EQ(deps.at("obj one.o"),
        (std::unordered_set<std::string> { "src one.c", "include/a.h", "include/space header.h", "include/dollar$.h" }));
    ASSERT_TRUE(deps.contains("other.o"));
    EXPECT_EQ(deps.at("other.o"), (std::unordered_set<std::string> { "other.c", "include/a.h" }));
}
