#pragma once

#include <cstdint>
#include <optional>
#include <cstddef>
#include <string>
#include <string_view>

namespace wallpaper::shader_lex
{

inline bool IsHSpace(char c) { return c == ' ' || c == '\t'; }
inline bool IsVSpace(char c) { return c == '\n' || c == '\r'; }
inline bool IsIdStart(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
inline bool IsIdCont(char c) {
    return IsIdStart(c) || (c >= '0' && c <= '9');
}
inline bool IsDigit(char c) { return c >= '0' && c <= '9'; }

class Cursor {
public:
    explicit Cursor(std::string_view source) noexcept
        : source_(source)
    {}
    Cursor(std::string_view source, std::size_t pos) noexcept
        : source_(source)
        , pos_(pos)
    {}

    std::size_t Pos() const noexcept { return pos_; }
    std::string_view Source() const noexcept { return source_; }
    bool Eof() const noexcept { return pos_ >= source_.size(); }
    char Peek(std::size_t offset = 0) const noexcept {
        return pos_ + offset < source_.size() ? source_[pos_ + offset] : '\0';
    }
    void SeekTo(std::size_t pos) noexcept { pos_ = pos > source_.size() ? source_.size() : pos; }
    void Advance(std::size_t count = 1) noexcept { SeekTo(pos_ + count); }

    bool AtLineStart() const noexcept { return pos_ == 0 || source_[pos_ - 1] == '\n'; }
    std::size_t LineStart() const noexcept {
        if (pos_ == 0) return 0;
        const auto newline = source_.rfind('\n', pos_ - 1);
        return newline == std::string_view::npos ? 0 : newline + 1;
    }
    std::size_t LineEnd() const noexcept {
        const auto newline = source_.find('\n', pos_);
        return newline == std::string_view::npos ? source_.size() : newline;
    }
    std::string_view CurrentLine() const noexcept {
        const std::size_t start = LineStart();
        const std::size_t end = LineEnd();
        return source_.substr(start, end - start);
    }
    void SkipLine() noexcept {
        const std::size_t end = LineEnd();
        SeekTo(end < source_.size() ? end + 1 : end);
    }

    void SkipHSpace() noexcept {
        while (pos_ < source_.size() && IsHSpace(source_[pos_])) {
            pos_++;
        }
    }
    void SkipToEol() noexcept {
        while (pos_ < source_.size() && source_[pos_] != '\n') {
            pos_++;
        }
    }
    void SkipAllTrivia() noexcept {
        while (pos_ < source_.size()) {
            const char ch = source_[pos_];
            if (IsHSpace(ch) || IsVSpace(ch)) {
                pos_++;
                continue;
            }
            if (ch == '/' && pos_ + 1 < source_.size()) {
                if (source_[pos_ + 1] == '/') {
                    SkipToEol();
                    continue;
                }
                if (source_[pos_ + 1] == '*') {
                    pos_ += 2;
                    while (
                        pos_ + 1 < source_.size() &&
                        !(source_[pos_] == '*' && source_[pos_ + 1] == '/')) {
                        pos_++;
                    }
                    pos_ = pos_ + 1 < source_.size() ? pos_ + 2 : source_.size();
                    continue;
                }
            }
            break;
        }
    }

    std::optional<std::string_view> ReadIdent() noexcept {
        if (pos_ >= source_.size() || !IsIdStart(source_[pos_])) {
            return std::nullopt;
        }
        const std::size_t start = pos_++;
        while (pos_ < source_.size() && IsIdCont(source_[pos_])) {
            pos_++;
        }
        return source_.substr(start, pos_ - start);
    }
    std::optional<std::string_view> ReadInt() noexcept {
        if (pos_ >= source_.size() || !IsDigit(source_[pos_])) {
            return std::nullopt;
        }
        const std::size_t start = pos_++;
        while (pos_ < source_.size() && IsDigit(source_[pos_])) {
            pos_++;
        }
        return source_.substr(start, pos_ - start);
    }
    std::optional<std::string_view> ReadArraySuffix() noexcept {
        if (pos_ >= source_.size() || source_[pos_] != '[') {
            return std::nullopt;
        }
        const std::size_t start = pos_;
        std::size_t end = pos_ + 1;
        while (end < source_.size() && source_[end] != ']' && source_[end] != '\n') {
            end++;
        }
        if (end >= source_.size() || source_[end] != ']') {
            return std::nullopt;
        }
        end++;
        pos_ = end;
        return source_.substr(start, end - start);
    }

    bool MatchChar(char ch) noexcept {
        if (pos_ < source_.size() && source_[pos_] == ch) {
            pos_++;
            return true;
        }
        return false;
    }
    bool MatchPunct(std::string_view value) noexcept {
        if (pos_ + value.size() > source_.size()) return false;
        if (source_.substr(pos_, value.size()) != value) return false;
        pos_ += value.size();
        return true;
    }
    bool MatchKeyword(std::string_view keyword) noexcept {
        if (pos_ + keyword.size() > source_.size()) return false;
        if (source_.substr(pos_, keyword.size()) != keyword) return false;
        if (pos_ + keyword.size() < source_.size() && IsIdCont(source_[pos_ + keyword.size()])) {
            return false;
        }
        pos_ += keyword.size();
        return true;
    }
    bool MatchHashDirective(std::string_view name) noexcept {
        const std::size_t save = pos_;
        SkipHSpace();
        if (pos_ >= source_.size() || source_[pos_] != '#') {
            pos_ = save;
            return false;
        }
        pos_++;
        SkipHSpace();
        if (!MatchKeyword(name)) {
            pos_ = save;
            return false;
        }
        return true;
    }

    struct Saved {
        std::size_t pos;
    };
    Saved Save() const noexcept { return { pos_ }; }
    void Restore(Saved saved) noexcept { pos_ = saved.pos; }

private:
    std::string_view source_;
    std::size_t pos_ { 0 };
};

class LineWalker {
public:
    explicit LineWalker(std::string_view source) noexcept
        : source_(source)
    {
        Recompute();
    }

    bool Done() const noexcept { return pos_ > source_.size(); }
    std::size_t LineStart() const noexcept { return line_start_; }
    std::size_t LineEnd() const noexcept { return line_end_; }
    std::string_view Line() const noexcept {
        return visible_line_;
    }
    std::string_view RawLine() const noexcept {
        return source_.substr(line_start_, line_end_ - line_start_);
    }
    Cursor LineCursor() const noexcept { return Cursor { Line() }; }
    void Step() noexcept {
        pos_ = line_end_ < source_.size() ? line_end_ + 1 : source_.size() + 1;
        Recompute();
    }

private:
    void Recompute() noexcept {
        if (pos_ > source_.size()) {
            line_start_ = source_.size();
            line_end_ = source_.size();
            visible_line_ = {};
            return;
        }

        line_start_ = pos_;
        const auto newline = source_.find('\n', pos_);
        line_end_ = newline == std::string_view::npos ? source_.size() : newline;

        visible_line_storage_.clear();
        bool used_storage = false;
        std::size_t visible_segment_start = line_start_;
        std::size_t scan = line_start_;
        while (scan < line_end_) {
            if (in_block_) {
                if (scan + 1 < line_end_ && source_[scan] == '*' && source_[scan + 1] == '/') {
                    used_storage = true;
                    in_block_ = false;
                    scan += 2;
                    visible_segment_start = scan;
                } else {
                    scan++;
                }
            } else {
                if (scan + 1 < line_end_ && source_[scan] == '/' && source_[scan + 1] == '*') {
                    used_storage = true;
                    visible_line_storage_.append(
                        source_.substr(visible_segment_start, scan - visible_segment_start));
                    in_block_ = true;
                    scan += 2;
                } else if (scan + 1 < line_end_ && source_[scan] == '/' && source_[scan + 1] == '/') {
                    scan = line_end_;
                    break;
                } else {
                    scan++;
                }
            }
        }

        if (used_storage) {
            if (!in_block_) {
                visible_line_storage_.append(
                    source_.substr(visible_segment_start, scan - visible_segment_start));
            }
            visible_line_ = visible_line_storage_;
        } else if (in_block_) {
            visible_line_ = {};
        } else {
            visible_line_ = source_.substr(line_start_, scan - line_start_);
        }
    }

    std::string_view source_;
    std::size_t pos_ { 0 };
    std::size_t line_start_ { 0 };
    std::size_t line_end_ { 0 };
    std::string visible_line_storage_;
    std::string_view visible_line_;
    bool in_block_ { false };
};

enum class TokenKind : std::uint8_t {
    Eof,
    Newline,
    HSpace,
    LineComment,
    BlockComment,
    Ident,
    Int,
    String,
    Hash,
    Punct,
    Unknown,
};

struct Token {
    TokenKind kind;
    std::string_view text;
    std::size_t offset;
};

class Lexer {
public:
    explicit Lexer(std::string_view source) noexcept
        : source_(source)
    {}

    std::string_view Source() const noexcept { return source_; }
    std::size_t Pos() const noexcept { return pos_; }
    void SeekTo(std::size_t pos) noexcept { pos_ = pos > source_.size() ? source_.size() : pos; }
    bool Eof() const noexcept { return pos_ >= source_.size(); }

    Token Peek() const noexcept {
        const std::size_t save = pos_;
        Token token = const_cast<Lexer*>(this)->ScanOne();
        const_cast<Lexer*>(this)->pos_ = save;
        return token;
    }
    Token Next() noexcept { return ScanOne(); }

    template<typename Pred>
    Token NextSkip(Pred pred) noexcept {
        for (;;) {
            Token token = ScanOne();
            if (!pred(token.kind)) {
                return token;
            }
            if (token.kind == TokenKind::Eof) {
                return token;
            }
        }
    }

    struct Saved {
        std::size_t pos;
    };
    Saved Save() const noexcept { return { pos_ }; }
    void Restore(Saved saved) noexcept { pos_ = saved.pos; }

private:
    Token ScanOne() noexcept {
        if (pos_ >= source_.size()) {
            return { TokenKind::Eof, {}, source_.size() };
        }

        const std::size_t start = pos_;
        const char ch = source_[pos_];
        if (ch == '\n') {
            pos_++;
            return { TokenKind::Newline, source_.substr(start, 1), start };
        }
        if (IsHSpace(ch)) {
            while (pos_ < source_.size() && IsHSpace(source_[pos_])) {
                pos_++;
            }
            return { TokenKind::HSpace, source_.substr(start, pos_ - start), start };
        }
        if (ch == '/' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '/') {
            pos_ += 2;
            while (pos_ < source_.size() && source_[pos_] != '\n') {
                pos_++;
            }
            return { TokenKind::LineComment, source_.substr(start, pos_ - start), start };
        }
        if (ch == '/' && pos_ + 1 < source_.size() && source_[pos_ + 1] == '*') {
            pos_ += 2;
            while (
                pos_ + 1 < source_.size() &&
                !(source_[pos_] == '*' && source_[pos_ + 1] == '/')) {
                pos_++;
            }
            pos_ = pos_ + 1 < source_.size() ? pos_ + 2 : source_.size();
            return { TokenKind::BlockComment, source_.substr(start, pos_ - start), start };
        }
        if (IsIdStart(ch)) {
            pos_++;
            while (pos_ < source_.size() && IsIdCont(source_[pos_])) {
                pos_++;
            }
            return { TokenKind::Ident, source_.substr(start, pos_ - start), start };
        }
        if (IsDigit(ch)) {
            pos_++;
            while (pos_ < source_.size() && IsDigit(source_[pos_])) {
                pos_++;
            }
            return { TokenKind::Int, source_.substr(start, pos_ - start), start };
        }
        if (ch == '"') {
            pos_++;
            while (pos_ < source_.size() && source_[pos_] != '"' && source_[pos_] != '\n') {
                pos_++;
            }
            if (pos_ < source_.size() && source_[pos_] == '"') {
                pos_++;
            }
            return { TokenKind::String, source_.substr(start, pos_ - start), start };
        }
        if (ch == '#') {
            pos_++;
            return { TokenKind::Hash, source_.substr(start, 1), start };
        }
        if (static_cast<unsigned char>(ch) >= 0x20 && static_cast<unsigned char>(ch) < 0x7f) {
            pos_++;
            return { TokenKind::Punct, source_.substr(start, 1), start };
        }
        pos_++;
        return { TokenKind::Unknown, source_.substr(start, 1), start };
    }

    std::string_view source_;
    std::size_t pos_ { 0 };
};

enum class PpKind {
    None,
    If,
    Ifdef,
    Ifndef,
    Elif,
    Else,
    Endif,
    Define,
    Undef,
    Include,
    Require,
    Pragma,
    Extension,
    Version,
    Other,
};

inline PpKind ClassifyPreproc(Cursor cursor) noexcept {
    Lexer lexer(cursor.Source().substr(cursor.Pos()));
    Token token = lexer.NextSkip([](TokenKind kind) { return kind == TokenKind::HSpace; });
    if (token.kind != TokenKind::Hash) {
        return PpKind::None;
    }
    token = lexer.NextSkip([](TokenKind kind) { return kind == TokenKind::HSpace; });
    if (token.kind != TokenKind::Ident) {
        return PpKind::Other;
    }

    const auto id = token.text;
    if (id == "if") return PpKind::If;
    if (id == "ifdef") return PpKind::Ifdef;
    if (id == "ifndef") return PpKind::Ifndef;
    if (id == "elif") return PpKind::Elif;
    if (id == "else") return PpKind::Else;
    if (id == "endif") return PpKind::Endif;
    if (id == "define") return PpKind::Define;
    if (id == "undef") return PpKind::Undef;
    if (id == "include") return PpKind::Include;
    if (id == "require") return PpKind::Require;
    if (id == "pragma") return PpKind::Pragma;
    if (id == "extension") return PpKind::Extension;
    if (id == "version") return PpKind::Version;
    return PpKind::Other;
}

} // namespace wallpaper::shader_lex
