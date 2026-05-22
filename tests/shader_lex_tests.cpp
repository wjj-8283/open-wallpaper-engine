#include "Scene/Parse/ShaderLex.hpp"

#include <gtest/gtest.h>

using namespace wallpaper::shader_lex;

TEST(ShaderLex, CharClass) {
    EXPECT_TRUE(IsHSpace(' '));
    EXPECT_TRUE(IsHSpace('\t'));
    EXPECT_FALSE(IsHSpace('\n'));
    EXPECT_TRUE(IsVSpace('\n'));
    EXPECT_TRUE(IsVSpace('\r'));
    EXPECT_FALSE(IsVSpace(' '));
    EXPECT_TRUE(IsIdStart('_'));
    EXPECT_TRUE(IsIdStart('a'));
    EXPECT_TRUE(IsIdStart('Z'));
    EXPECT_FALSE(IsIdStart('9'));
    EXPECT_FALSE(IsIdStart('-'));
    EXPECT_TRUE(IsIdCont('0'));
    EXPECT_TRUE(IsIdCont('_'));
    EXPECT_TRUE(IsDigit('0'));
    EXPECT_TRUE(IsDigit('9'));
    EXPECT_FALSE(IsDigit('a'));
}

TEST(ShaderLex, SkipHSpaceDoesNotCrossNewline) {
    Cursor c("  \t  \n   abc");
    c.SkipHSpace();
    EXPECT_EQ(c.Pos(), 5u);
    EXPECT_EQ(c.Peek(), '\n');
}

TEST(ShaderLex, ReadIdent) {
    {
        Cursor c("abc123 rest");
        auto id = c.ReadIdent();
        ASSERT_TRUE(id);
        EXPECT_EQ(*id, "abc123");
    }
    {
        Cursor c("_x");
        auto id = c.ReadIdent();
        ASSERT_TRUE(id);
        EXPECT_EQ(*id, "_x");
    }
    {
        Cursor c("9bad");
        EXPECT_FALSE(c.ReadIdent().has_value());
        EXPECT_EQ(c.Pos(), 0u);
    }
    {
        Cursor c("");
        EXPECT_FALSE(c.ReadIdent().has_value());
    }
}

TEST(ShaderLex, MatchKeywordRespectsIdentBoundary) {
    {
        Cursor c("uniform vec4");
        EXPECT_TRUE(c.MatchKeyword("uniform"));
        EXPECT_EQ(c.Pos(), 7u);
    }
    {
        Cursor c("uniformly");
        EXPECT_FALSE(c.MatchKeyword("uniform"));
        EXPECT_EQ(c.Pos(), 0u);
    }
    {
        Cursor c("uniform_x");
        EXPECT_FALSE(c.MatchKeyword("uniform"));
    }
    {
        Cursor c("uniform");
        EXPECT_TRUE(c.MatchKeyword("uniform"));
    }
}

TEST(ShaderLex, ReadArraySuffix) {
    {
        Cursor c("[42] rest");
        auto a = c.ReadArraySuffix();
        ASSERT_TRUE(a);
        EXPECT_EQ(*a, "[42]");
        EXPECT_EQ(c.Pos(), 4u);
    }
    {
        Cursor c("[]");
        auto a = c.ReadArraySuffix();
        ASSERT_TRUE(a);
        EXPECT_EQ(*a, "[]");
    }
    {
        Cursor c("[N+1]");
        auto a = c.ReadArraySuffix();
        ASSERT_TRUE(a);
        EXPECT_EQ(*a, "[N+1]");
    }
    {
        Cursor c("nope");
        EXPECT_FALSE(c.ReadArraySuffix().has_value());
        EXPECT_EQ(c.Pos(), 0u);
    }
}

TEST(ShaderLex, SkipAllTriviaEatsBlockComment) {
    {
        Cursor c("/* abc */xy");
        c.SkipAllTrivia();
        EXPECT_EQ(c.Pos(), 9u);
        EXPECT_EQ(c.Peek(), 'x');
    }
    {
        Cursor c("  /* a\n b */ z");
        c.SkipAllTrivia();
        EXPECT_EQ(c.Peek(), 'z');
    }
    {
        Cursor c("// trailing");
        c.SkipAllTrivia();
        EXPECT_TRUE(c.Eof());
    }
}

TEST(ShaderLex, MatchHashDirective) {
    {
        Cursor c("#include \"x\"");
        EXPECT_TRUE(c.MatchHashDirective("include"));
        EXPECT_EQ(c.Peek(), ' ');
    }
    {
        Cursor c("  #  include \"x\"");
        EXPECT_TRUE(c.MatchHashDirective("include"));
    }
    {
        Cursor c("# require X");
        EXPECT_TRUE(c.MatchHashDirective("require"));
    }
    {
        Cursor c("//#include \"x\"");
        EXPECT_FALSE(c.MatchHashDirective("include"));
        EXPECT_EQ(c.Pos(), 0u);
    }
}

TEST(ShaderLex, LineWalkerBasic) {
    std::string_view src = "line1\nline2\nline3";
    LineWalker w(src);
    ASSERT_FALSE(w.Done());
    EXPECT_EQ(w.Line(), "line1");
    EXPECT_EQ(w.LineStart(), 0u);
    EXPECT_EQ(w.LineEnd(), 5u);
    w.Step();
    EXPECT_EQ(w.Line(), "line2");
    w.Step();
    EXPECT_EQ(w.Line(), "line3");
    w.Step();
    EXPECT_TRUE(w.Done());
}

TEST(ShaderLex, LineWalkerEmptyLines) {
    LineWalker w("a\n\nb");
    EXPECT_EQ(w.Line(), "a");
    w.Step();
    EXPECT_EQ(w.Line(), "");
    w.Step();
    EXPECT_EQ(w.Line(), "b");
    w.Step();
    EXPECT_TRUE(w.Done());
}

TEST(ShaderLex, LineWalkerMasksBlockCommentLines) {
    std::string_view src =
        "alpha\n"
        "/* hidden\n"
        " uniform vec4 g_X;\n"
        "*/\n"
        "after";
    LineWalker w(src);
    EXPECT_EQ(w.Line(), "alpha");
    w.Step();
    EXPECT_EQ(w.Line().find("uniform"), std::string_view::npos);
    w.Step();
    EXPECT_TRUE(w.Line().empty());
    w.Step();
    w.Step();
    EXPECT_EQ(w.Line(), "after");
}

TEST(ShaderLex, LineWalkerMasksLineAfterMidLineBlockCommentStart) {
    std::string_view src =
        "float ignored; /* // [COMBO] {\"combo\":\"HIDDEN\",\"default\":1}\n"
        "*/\n";
    LineWalker w(src);
    EXPECT_EQ(w.Line(), "float ignored; ");
    w.Step();
    EXPECT_TRUE(w.Line().empty());
}

TEST(ShaderLex, LineWalkerPreservesTextAfterSameLineBlockComment) {
    LineWalker w("/* note */ uniform float g_Value; // {\"default\":1}");
    EXPECT_EQ(w.Line(), " uniform float g_Value; // {\"default\":1}");
}

TEST(ShaderLex, LexerKinds) {
    Lexer lx("  uniform vec4 g_X[3]; // tail\n#include \"x.h\"\n/* blk */");
    auto t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::HSpace);
    EXPECT_EQ(t.text, "  ");
    t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::Ident);
    EXPECT_EQ(t.text, "uniform");
    t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::HSpace);
    t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::Ident);
    EXPECT_EQ(t.text, "vec4");
    t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::HSpace);
    t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::Ident);
    EXPECT_EQ(t.text, "g_X");
    t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::Punct);
    EXPECT_EQ(t.text, "[");
    t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::Int);
    EXPECT_EQ(t.text, "3");
    t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::Punct);
    EXPECT_EQ(t.text, "]");
    t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::Punct);
    EXPECT_EQ(t.text, ";");
    t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::HSpace);
    t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::LineComment);
    EXPECT_EQ(t.text, "// tail");
    t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::Newline);
    t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::Hash);
    t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::Ident);
    EXPECT_EQ(t.text, "include");
    t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::HSpace);
    t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::String);
    EXPECT_EQ(t.text, "\"x.h\"");
    t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::Newline);
    t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::BlockComment);
    EXPECT_EQ(t.text, "/* blk */");
    t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::Eof);
}

TEST(ShaderLex, LexerPeekDoesNotAdvance) {
    Lexer lx("abc def");
    auto p1 = lx.Peek();
    auto p2 = lx.Peek();
    EXPECT_EQ(p1.kind, p2.kind);
    EXPECT_EQ(p1.offset, p2.offset);
    EXPECT_EQ(p1.text, "abc");
    auto n = lx.Next();
    EXPECT_EQ(n.text, "abc");
    EXPECT_EQ(lx.Peek().kind, TokenKind::HSpace);
}

TEST(ShaderLex, LexerSaveRestore) {
    Lexer lx("alpha beta");
    (void)lx.Next();
    auto s = lx.Save();
    auto t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::HSpace);
    lx.Restore(s);
    EXPECT_EQ(lx.Next().kind, TokenKind::HSpace);
}

TEST(ShaderLex, LexerUnterminatedStringTerminatesAtEol) {
    Lexer lx("\"oops\n");
    auto t = lx.Next();
    EXPECT_EQ(t.kind, TokenKind::String);
    EXPECT_EQ(t.text, "\"oops");
    EXPECT_EQ(lx.Next().kind, TokenKind::Newline);
}

TEST(ShaderLex, ClassifyPreproc) {
    auto cls = [](std::string_view s) { return ClassifyPreproc(Cursor(s)); };
    EXPECT_EQ(cls("#if X"), PpKind::If);
    EXPECT_EQ(cls("#ifdef X"), PpKind::Ifdef);
    EXPECT_EQ(cls("  # endif"), PpKind::Endif);
    EXPECT_EQ(cls("#define X 1"), PpKind::Define);
    EXPECT_EQ(cls("#require X"), PpKind::Require);
    EXPECT_EQ(cls("#include \"x\""), PpKind::Include);
    EXPECT_EQ(cls("int x;"), PpKind::None);
    EXPECT_EQ(cls("#unknown"), PpKind::Other);
}
