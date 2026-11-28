#include <gtest/gtest.h>
#include <tree_sitter/api.h>
#include <tree_sitter/tree-sitter-bash.h>

#include <string>
#include <string_view>
#include <vector>

#include "nixja_gen.hpp"

class TreeSitterBash : public ::testing::Test {
protected:
    TSParser* parser = nullptr;
    TSTree* tree = nullptr;

    void SetUp() override {
        parser = ts_parser_new();

        ASSERT_NE(parser, nullptr);
        ASSERT_TRUE(ts_parser_set_language(parser, tree_sitter_bash()));
    }

    void TearDown() override {
        if (tree)
            ts_tree_delete(tree);

        if (parser)
            ts_parser_delete(parser);
    }

    void expect_commands(
        std::string_view source,
        const std::vector<CommandInvocation>& expected) {

        tree = ts_parser_parse_string(
            parser,
            nullptr,
            source.data(),
            source.size());

        ASSERT_NE(tree, nullptr);

        const auto root = ts_tree_root_node(tree);

        ASSERT_FALSE(ts_node_has_error(root));

        std::vector<CommandInvocation> commands;

        collect_commands(root, source, commands);

        ASSERT_EQ(commands.size(), expected.size());

        for (std::size_t i = 0; i < commands.size(); ++i) {
            SCOPED_TRACE(i);

            EXPECT_EQ(commands[i].program, expected[i].program);
            EXPECT_EQ(commands[i].command, expected[i].command);
            EXPECT_EQ(commands[i].program_offset, expected[i].program_offset);

            EXPECT_EQ(
                source.substr(
                    commands[i].program_offset,
                    commands[i].program.size()),
                commands[i].program);
        }
    }
};

TEST_F(TreeSitterBash, SingleCommand) {
    expect_commands("gcc -o main main.c", {
                                              { "gcc", "gcc -o main main.c", 0 },
                                          });
}

TEST_F(TreeSitterBash, BackgroundCommands) {
    expect_commands("gcc -c main.c & echo hello", {
                                                      { "gcc", "gcc -c main.c", 0 },
                                                      { "echo", "echo hello", 16 },
                                                  });
}

TEST_F(TreeSitterBash, SequentialCommands) {
    expect_commands("gcc main.c; echo done", {
                                                 { "gcc", "gcc main.c", 0 },
                                                 { "echo", "echo done", 12 },
                                             });
}

TEST_F(TreeSitterBash, LogicalAnd) {
    expect_commands("gcc main.c && ./a.out", {
                                                 { "gcc", "gcc main.c", 0 },
                                                 { "./a.out", "./a.out", 14 },
                                             });
}

TEST_F(TreeSitterBash, LogicalOr) {
    expect_commands("gcc main.c || echo failed", {
                                                     { "gcc", "gcc main.c", 0 },
                                                     { "echo", "echo failed", 14 },
                                                 });
}

TEST_F(TreeSitterBash, Pipeline) {
    expect_commands("cat main.c | grep hello", {
                                                   { "cat", "cat main.c", 0 },
                                                   { "grep", "grep hello", 13 },
                                               });
}

TEST_F(TreeSitterBash, VariableExpansion) {
    expect_commands("$CC -c main.c", {
                                         { "$CC", "$CC -c main.c", 0 },
                                     });
}

TEST_F(TreeSitterBash, BracedVariableExpansion) {
    expect_commands("${CC} -c main.c", {
                                           { "${CC}", "${CC} -c main.c", 0 },
                                       });
}

TEST_F(TreeSitterBash, QuotedVariableExpansion) {
    expect_commands("\"$CC\" -c main.c", {
                                             { "\"$CC\"", "\"$CC\" -c main.c", 0 },
                                         });
}

TEST_F(TreeSitterBash, QuotedArguments) {
    expect_commands("echo \"hello world\"", {
                                                { "echo", "echo \"hello world\"", 0 },
                                            });
}

TEST_F(TreeSitterBash, AbsoluteExecutable) {
    expect_commands("/usr/bin/gcc main.c", {
                                               { "/usr/bin/gcc", "/usr/bin/gcc main.c", 0 },
                                           });
}

TEST_F(TreeSitterBash, RelativeExecutable) {
    expect_commands("./scripts/generate input.txt", {
                                                        { "./scripts/generate", "./scripts/generate input.txt", 0 },
                                                    });
}

TEST_F(TreeSitterBash, CMakeLinkCommand) {
    expect_commands(": && gcc main.o -o main && :", {
                                                        { ":", ":", 0 },
                                                        { "gcc", "gcc main.o -o main", 5 },
                                                        { ":", ":", 27 },
                                                    });
}

TEST_F(TreeSitterBash, DuplicateCommands) {
    expect_commands("echo hello; echo hello", {
                                                  { "echo", "echo hello", 0 },
                                                  { "echo", "echo hello", 12 },
                                              });
}

TEST_F(TreeSitterBash, EmptyCommand) {
    expect_commands("", { });
}

TEST_F(TreeSitterBash, WhitespaceOnly) {
    expect_commands(" \t\n", { });
}

TEST_F(TreeSitterBash, LeadingWhitespace) {
    expect_commands("   gcc main.c", {
                                         { "gcc", "gcc main.c", 3 },
                                     });
}

TEST_F(TreeSitterBash, CommandSubstitution) {
    expect_commands("echo $(date)", {
                                        { "echo", "echo $(date)", 0 },
                                        { "date", "date", 7 },
                                    });
}

TEST_F(TreeSitterBash, ComplexCommand) {
    expect_commands("$PRE_LINK & gcc -o main main.c & echo \"hello\"", {
                                                                           { "$PRE_LINK", "$PRE_LINK", 0 },
                                                                           { "gcc", "gcc -o main main.c", 12 },
                                                                           { "echo", "echo \"hello\"", 33 },
                                                                       });
}
