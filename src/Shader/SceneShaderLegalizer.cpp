#include "Shader/SceneShaderLegalizer.hpp"
#include "Utils/Logging.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace
{

template<typename Fn>
[[nodiscard]] std::string ApplyLegalizerPass(const char* name, std::string source, Fn&& fn)
{
    try {
        return fn(std::move(source));
    } catch (const std::regex_error& error) {
        LOG_ERROR("shader legalizer pass %s failed: %s", name, error.what());
        return source;
    }
}

struct ConditionalFrame
{
    bool parent_active { true };
    bool current_active { true };
    bool any_branch_taken { false };
};

class ExpressionParser
{
public:
    ExpressionParser(std::string expression, const std::map<std::string, int64_t>& macros)
        : m_expression(std::move(expression))
        , m_macros(macros)
    {
    }

    [[nodiscard]] bool parse()
    {
        const bool result = parseOr();
        skipWhitespace();
        return result;
    }

private:
    [[nodiscard]] bool parseOr()
    {
        bool result = parseAnd();

        while (true) {
            skipWhitespace();
            if (!consume("||")) {
                return result;
            }
            result = parseAnd() || result;
        }
    }

    [[nodiscard]] bool parseAnd()
    {
        bool result = parseEquality();

        while (true) {
            skipWhitespace();
            if (!consume("&&")) {
                return result;
            }
            result = parseEquality() && result;
        }
    }

    [[nodiscard]] bool parseEquality()
    {
        int64_t left = parseValue();

        while (true) {
            skipWhitespace();

            if (consume("==")) {
                left = left == parseValue();
            } else if (consume("!=")) {
                left = left != parseValue();
            } else if (consume(">=")) {
                left = left >= parseValue();
            } else if (consume("<=")) {
                left = left <= parseValue();
            } else if (consume(">")) {
                left = left > parseValue();
            } else if (consume("<")) {
                left = left < parseValue();
            } else {
                return left != 0;
            }
        }
    }

    [[nodiscard]] int64_t parseValue()
    {
        skipWhitespace();

        if (consume("!")) {
            return parseValue() == 0 ? 1 : 0;
        }

        return parsePrimary();
    }

    [[nodiscard]] int64_t parsePrimary()
    {
        skipWhitespace();

        if (consume("(")) {
            const bool result = parseOr();
            (void)consume(")");
            return result ? 1 : 0;
        }

        if (m_offset >= m_expression.size()) {
            return 0;
        }

        if (std::isdigit(static_cast<unsigned char>(m_expression[m_offset]))) {
            char* end = nullptr;
            const int64_t value = std::strtoll(m_expression.c_str() + m_offset, &end, 10);
            m_offset = static_cast<size_t>(end - m_expression.c_str());
            return value;
        }

        if (std::isalpha(static_cast<unsigned char>(m_expression[m_offset])) ||
            m_expression[m_offset] == '_') {
            const size_t begin = m_offset++;

            while (m_offset < m_expression.size() &&
                   (std::isalnum(static_cast<unsigned char>(m_expression[m_offset])) ||
                    m_expression[m_offset] == '_')) {
                m_offset++;
            }

            const auto macro =
                m_macros.find(m_expression.substr(begin, m_offset - begin));
            return macro != m_macros.end() ? macro->second : 0;
        }

        m_offset++;
        return 0;
    }

    void skipWhitespace()
    {
        while (m_offset < m_expression.size() &&
               std::isspace(static_cast<unsigned char>(m_expression[m_offset]))) {
            m_offset++;
        }
    }

    [[nodiscard]] bool consume(const std::string_view token)
    {
        skipWhitespace();

        if (m_expression.size() - m_offset < token.size() ||
            m_expression.compare(m_offset, token.size(), token) != 0) {
            return false;
        }

        m_offset += token.size();
        return true;
    }

    std::string m_expression;
    const std::map<std::string, int64_t>& m_macros;
    size_t m_offset { 0 };
};

[[nodiscard]] std::string trimCopy(std::string value)
{
    const auto begin = std::ranges::find_if(value, [] (const unsigned char ch) {
        return !std::isspace(ch);
    });
    const auto end = std::find_if(value.rbegin(), value.rend(), [] (const unsigned char ch) {
        return !std::isspace(ch);
    }).base();

    if (begin >= end) {
        return {};
    }

    return { begin, end };
}

[[nodiscard]] bool startsWith(const std::string& value, const std::string_view prefix)
{
    return value.size() >= prefix.size() &&
           std::string_view(value).substr(0, prefix.size()) == prefix;
}

[[nodiscard]] bool isIdentifierChar(const char ch)
{
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

[[nodiscard]] bool isActive(const std::vector<ConditionalFrame>& conditionals)
{
    return conditionals.empty() || conditionals.back().current_active;
}

[[nodiscard]] bool evaluateCondition(
    const std::string& expression,
    const std::map<std::string, int64_t>& macros)
{
    return ExpressionParser(expression, macros).parse();
}

void recordMacro(const std::string& trimmed, std::map<std::string, int64_t>& macros)
{
    static const std::regex define_pattern(
        R"(^#define\s+([A-Za-z_][A-Za-z0-9_]*)(?:\s+([-+]?[0-9]+))?.*$)");
    std::smatch match;

    if (!std::regex_match(trimmed, match, define_pattern)) {
        return;
    }

    macros[match[1].str()] = match[2].matched ? std::stoll(match[2].str()) : 1;
}

[[nodiscard]] bool handleConditionalDirective(
    const std::string& trimmed,
    const std::map<std::string, int64_t>& macros,
    std::vector<ConditionalFrame>& conditionals)
{
    if (startsWith(trimmed, "#if ")) {
        const bool parent_active = isActive(conditionals);
        const bool condition_active = evaluateCondition(trimmed.substr(4), macros);
        conditionals.push_back(
            {
                .parent_active = parent_active,
                .current_active = parent_active && condition_active,
                .any_branch_taken = condition_active,
            });
        return true;
    }

    if (startsWith(trimmed, "#ifdef ")) {
        const std::string name = trimCopy(trimmed.substr(7));
        const bool parent_active = isActive(conditionals);
        const bool condition_active = macros.contains(name);
        conditionals.push_back(
            {
                .parent_active = parent_active,
                .current_active = parent_active && condition_active,
                .any_branch_taken = condition_active,
            });
        return true;
    }

    if (startsWith(trimmed, "#ifndef ")) {
        const std::string name = trimCopy(trimmed.substr(8));
        const bool parent_active = isActive(conditionals);
        const bool condition_active = !macros.contains(name);
        conditionals.push_back(
            {
                .parent_active = parent_active,
                .current_active = parent_active && condition_active,
                .any_branch_taken = condition_active,
            });
        return true;
    }

    if (startsWith(trimmed, "#elif ")) {
        if (!conditionals.empty()) {
            auto& frame = conditionals.back();
            const bool condition_active =
                !frame.any_branch_taken && evaluateCondition(trimmed.substr(6), macros);
            frame.current_active = frame.parent_active && condition_active;
            frame.any_branch_taken = frame.any_branch_taken || condition_active;
        }
        return true;
    }

    if (startsWith(trimmed, "#else")) {
        if (!conditionals.empty()) {
            auto& frame = conditionals.back();
            const bool condition_active = !frame.any_branch_taken;
            frame.current_active = frame.parent_active && condition_active;
            frame.any_branch_taken = true;
        }
        return true;
    }

    if (startsWith(trimmed, "#endif")) {
        if (!conditionals.empty()) {
            conditionals.pop_back();
        }
        return true;
    }

    return false;
}

[[nodiscard]] std::string StripInactiveConditionals(
    std::string_view source,
    std::map<std::string, int64_t>& macros)
{
    std::stringstream input { std::string(source) };
    std::ostringstream output;
    std::string line;
    std::vector<ConditionalFrame> conditionals;

    while (std::getline(input, line)) {
        const std::string trimmed = trimCopy(line);

        if (startsWith(trimmed, "#define ")) {
            if (isActive(conditionals)) {
                recordMacro(trimmed, macros);
                output << line << '\n';
            }
            continue;
        }

        if (handleConditionalDirective(trimmed, macros, conditionals)) {
            continue;
        }

        if (isActive(conditionals)) {
            output << line << '\n';
        }
    }

    return output.str();
}

[[nodiscard]] wallpaper::usize FindMatchingParen(std::string_view source, wallpaper::usize open_paren_pos)
{
    int depth = 0;
    for (wallpaper::usize pos = open_paren_pos; pos < source.size(); pos++) {
        if (source[pos] == '(') {
            depth++;
        } else if (source[pos] == ')') {
            depth--;
            if (depth == 0) {
                return pos;
            }
        }
    }
    return std::string::npos;
}

[[nodiscard]] std::string RewriteTextureInitializerCast(std::string source, std::string_view vector_type)
{
    const std::string declaration = std::string(vector_type) + " ";
    const std::string cast_prefix = std::string(vector_type) + "(";
    wallpaper::usize search_pos = 0;

    while ((search_pos = source.find(declaration, search_pos)) != std::string::npos) {
        wallpaper::usize line_start = source.rfind('\n', search_pos);
        line_start = line_start == std::string::npos ? 0 : line_start + 1;

        const wallpaper::usize line_end = source.find(';', search_pos);
        if (line_end == std::string::npos) {
            break;
        }

        std::string line = source.substr(line_start, line_end - line_start + 1);
        const wallpaper::usize equal_pos = line.find('=');
        if (equal_pos == std::string::npos) {
            search_pos = line_end + 1;
            continue;
        }

        wallpaper::usize call_pos = line.find("texture(", equal_pos);
        if (call_pos == std::string::npos) {
            call_pos = line.find("texSample2D(", equal_pos);
        }
        if (call_pos == std::string::npos) {
            search_pos = line_end + 1;
            continue;
        }

        if (call_pos >= cast_prefix.size() &&
            line.substr(call_pos - cast_prefix.size(), cast_prefix.size()) == cast_prefix) {
            search_pos = line_end + 1;
            continue;
        }

        const wallpaper::usize open_paren = line.find('(', call_pos);
        const wallpaper::usize close_paren = FindMatchingParen(line, open_paren);
        if (close_paren == std::string::npos) {
            search_pos = line_end + 1;
            continue;
        }
        if (close_paren + 1 < line.size() && line[close_paren + 1] == '.') {
            search_pos = line_end + 1;
            continue;
        }

        line.insert(close_paren + 1, ")");
        line.insert(call_pos, cast_prefix);

        source.replace(line_start, line_end - line_start + 1, line);
        search_pos = line_start + line.size();
    }

    return source;
}

[[nodiscard]] std::string RewriteScalarTextureSampleInitializers(std::string source)
{
    const std::regex float_texture_init_re(
        R"(^(\s*float\s+[A-Za-z_]\w*\s*=\s*)((?:texSample2D|texSample2DLod|texture|textureLod)\s*\(.+\))(\s*;\r?)$)",
        std::regex::ECMAScript);

    wallpaper::usize search_pos = 0;
    while (search_pos < source.size()) {
        const wallpaper::usize line_end = source.find('\n', search_pos);
        const wallpaper::usize slice_end = line_end == std::string::npos ? source.size() : line_end;
        std::string line = source.substr(search_pos, slice_end - search_pos);
        std::smatch match;

        if (std::regex_match(line, match, float_texture_init_re)) {
            const std::string rhs = trimCopy(match[2].str());
            const wallpaper::usize open_paren = rhs.find('(');
            const wallpaper::usize close_paren = FindMatchingParen(rhs, open_paren);
            if (close_paren == rhs.size() - 1) {
                line = match[1].str() + rhs + ".r" + match[3].str();
                source.replace(search_pos, slice_end - search_pos, line);
                search_pos += line.size();
                continue;
            }
        }

        search_pos = line_end == std::string::npos ? source.size() : line_end + 1;
    }

    return source;
}

[[nodiscard]] std::vector<std::string> SplitTopLevelArgs(std::string_view source);

[[nodiscard]] std::string RewriteMultiVectorScalarDeclarations(std::string source)
{
    const std::unordered_map<std::string, int> vector_widths {
        { "vec2", 2 },
        { "vec3", 3 },
        { "vec4", 4 },
    };

    wallpaper::usize search_pos = 0;
    while (search_pos < source.size()) {
        const wallpaper::usize line_end = source.find('\n', search_pos);
        const wallpaper::usize slice_end = line_end == std::string::npos ? source.size() : line_end;
        std::string line = source.substr(search_pos, slice_end - search_pos);
        std::string trimmed = trimCopy(line);

        std::smatch match;
        static const std::regex decl_re(
            R"(^(\s*)(vec[234])\s+(.+);\r?$)",
            std::regex::ECMAScript);
        if (!std::regex_match(line, match, decl_re)) {
            search_pos = line_end == std::string::npos ? source.size() : line_end + 1;
            continue;
        }

        const std::string type_name = match[2].str();
        const auto width_it = vector_widths.find(type_name);
        if (width_it == vector_widths.end()) {
            search_pos = line_end == std::string::npos ? source.size() : line_end + 1;
            continue;
        }

        auto declarations = SplitTopLevelArgs(match[3].str());
        if (declarations.size() < 2) {
            search_pos = line_end == std::string::npos ? source.size() : line_end + 1;
            continue;
        }

        bool changed = false;
        for (auto& declaration : declarations) {
            const wallpaper::usize equal_pos = declaration.find('=');
            if (equal_pos == std::string::npos) {
                continue;
            }

            const std::string lhs = trimCopy(declaration.substr(0, equal_pos));
            const std::string rhs = trimCopy(declaration.substr(equal_pos + 1));
            if (lhs.empty() || rhs.empty()) {
                continue;
            }

            static const std::regex scalar_literal_re(
                R"(^[-+]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][-+]?[0-9]+)?$)",
                std::regex::ECMAScript);
            if (!std::regex_match(rhs, scalar_literal_re)) {
                continue;
            }

            declaration = lhs + " = " + type_name + "(" + rhs + ")";
            changed = true;
        }

        if (!changed) {
            search_pos = line_end == std::string::npos ? source.size() : line_end + 1;
            continue;
        }

        std::string rewritten = match[1].str() + type_name + " ";
        for (std::size_t i = 0; i < declarations.size(); ++i) {
            if (i != 0) rewritten += ", ";
            rewritten += declarations[i];
        }
        rewritten += ";";

        source.replace(search_pos, slice_end - search_pos, rewritten);
        search_pos += rewritten.size();
    }

    return source;
}

[[nodiscard]] std::string RewriteUserDefinedModFunction(std::string source)
{
    static const std::regex user_mod_re(
        R"((^|\n)\s*float\s+mod\s*\(\s*float\s+[A-Za-z_]\w*\s*,\s*float\s+[A-Za-z_]\w*\s*\))",
        std::regex::ECMAScript);
    if (!std::regex_search(source, user_mod_re)) {
        return source;
    }

    auto is_scalar_mod_call = [](std::string_view args_source) {
        const auto args = SplitTopLevelArgs(args_source);
        if (args.size() != 2) {
            return false;
        }
        static const std::regex scalar_literal_re(
            R"(^[-+]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][-+]?[0-9]+)?$)",
            std::regex::ECMAScript);
        static const std::regex scalar_constructor_re(
            R"(^(?:float|float1)\s*\(\s*[-+]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][-+]?[0-9]+)?\s*\)$)",
            std::regex::ECMAScript);
        for (const auto& arg : args) {
            const auto trimmed = trimCopy(arg);
            if (!std::regex_match(trimmed, scalar_literal_re) &&
                !std::regex_match(trimmed, scalar_constructor_re)) {
                return false;
            }
        }
        return true;
    };

    auto rewrite_code_segment = [&](std::string segment) {
        wallpaper::usize search_pos = 0;
        while (search_pos < segment.size()) {
            const wallpaper::usize mod_pos = segment.find("mod", search_pos);
            if (mod_pos == std::string::npos) {
                break;
            }

            const bool left_ok =
                mod_pos == 0 || !isIdentifierChar(segment[mod_pos - 1]);
            wallpaper::usize paren_pos = mod_pos + 3;
            while (paren_pos < segment.size() &&
                   std::isspace(static_cast<unsigned char>(segment[paren_pos]))) {
                paren_pos++;
            }
            const bool right_ok = paren_pos < segment.size() && segment[paren_pos] == '(';
            if (!left_ok || !right_ok) {
                search_pos = mod_pos + 3;
                continue;
            }

            const wallpaper::usize close_paren = FindMatchingParen(segment, paren_pos);
            if (close_paren == std::string::npos) {
                break;
            }
            const std::string prefix = trimCopy(segment.substr(0, mod_pos));
            const bool is_scalar_mod_declaration =
                prefix.size() >= 5 &&
                prefix.compare(prefix.size() - 5, 5, "float") == 0 &&
                (prefix.size() == 5 || !isIdentifierChar(prefix[prefix.size() - 6]));
            if (is_scalar_mod_declaration ||
                is_scalar_mod_call(segment.substr(paren_pos + 1, close_paren - paren_pos - 1))) {
                segment.replace(mod_pos, 3, "_ww_user_mod");
                search_pos = close_paren + std::string_view("_ww_user_").size() + 1;
            } else {
                search_pos = close_paren + 1;
            }
        }
        return segment;
    };

    std::ostringstream output;
    wallpaper::usize line_start = 0;
    bool in_block_comment = false;
    bool in_preprocessor_continuation = false;
    while (line_start < source.size()) {
        const wallpaper::usize line_end = source.find('\n', line_start);
        const wallpaper::usize slice_end = line_end == std::string::npos ? source.size() : line_end;
        std::string line = source.substr(line_start, slice_end - line_start);
        const std::string trimmed = trimCopy(line);

        const bool is_preprocessor_line = in_preprocessor_continuation || startsWith(trimmed, "#");
        if (is_preprocessor_line) {
            output << line;
            wallpaper::usize tail = line.size();
            while (tail > 0 && std::isspace(static_cast<unsigned char>(line[tail - 1]))) {
                tail--;
            }
            in_preprocessor_continuation = tail > 0 && line[tail - 1] == '\\';
        } else {
            std::string rewritten;
            wallpaper::usize pos = 0;
            while (pos < line.size()) {
                if (in_block_comment) {
                    const wallpaper::usize comment_end = line.find("*/", pos);
                    if (comment_end == std::string::npos) {
                        rewritten.append(line.substr(pos));
                        pos = line.size();
                    } else {
                        rewritten.append(line.substr(pos, comment_end - pos + 2));
                        pos = comment_end + 2;
                        in_block_comment = false;
                    }
                    continue;
                }

                const wallpaper::usize line_comment = line.find("//", pos);
                const wallpaper::usize block_comment = line.find("/*", pos);
                const wallpaper::usize comment_pos =
                    line_comment == std::string::npos
                        ? block_comment
                        : block_comment == std::string::npos
                            ? line_comment
                            : std::min(line_comment, block_comment);

                if (comment_pos == std::string::npos) {
                    rewritten.append(rewrite_code_segment(line.substr(pos)));
                    pos = line.size();
                    continue;
                }

                rewritten.append(rewrite_code_segment(line.substr(pos, comment_pos - pos)));
                if (comment_pos == line_comment) {
                    rewritten.append(line.substr(comment_pos));
                    pos = line.size();
                } else {
                    const wallpaper::usize comment_end = line.find("*/", comment_pos + 2);
                    if (comment_end == std::string::npos) {
                        rewritten.append(line.substr(comment_pos));
                        pos = line.size();
                        in_block_comment = true;
                    } else {
                        rewritten.append(line.substr(comment_pos, comment_end - comment_pos + 2));
                        pos = comment_end + 2;
                    }
                }
            }
            output << rewritten;
        }

        if (line_end == std::string::npos) {
            break;
        }
        output << '\n';
        line_start = line_end + 1;
    }

    return output.str();
}

[[nodiscard]] std::unordered_set<std::string> CollectVec3Identifiers(const std::string& source)
{
    std::unordered_set<std::string> identifiers;
    const std::regex vec3_re(
        R"((?:^|\n)\s*(?:in|out|uniform|varying|attribute)?\s*vec3\s+([A-Za-z_]\w*)\s*(?:[;=]))",
        std::regex::ECMAScript);
    for (auto it = std::sregex_iterator(source.begin(), source.end(), vec3_re);
         it != std::sregex_iterator();
         ++it) {
        identifiers.insert((*it)[1].str());
    }
    return identifiers;
}

[[nodiscard]] std::unordered_set<std::string> CollectVec4Identifiers(const std::string& source)
{
    std::unordered_set<std::string> identifiers;
    const std::regex vec4_re(
        R"((?:^|\n)\s*(?:in|out|uniform|varying|attribute)?\s*vec4\s+([A-Za-z_]\w*)\s*(?:[;=]))",
        std::regex::ECMAScript);
    for (auto it = std::sregex_iterator(source.begin(), source.end(), vec4_re);
         it != std::sregex_iterator();
         ++it) {
        identifiers.insert((*it)[1].str());
    }
    return identifiers;
}

[[nodiscard]] std::unordered_set<std::string> CollectVec2Identifiers(const std::string& source)
{
    std::unordered_set<std::string> identifiers;
    const std::regex vec2_re(
        R"((?:^|\n)\s*(?:in|out|uniform|varying|attribute)?\s*vec2\s+([A-Za-z_]\w*)\s*(?:[;=]))",
        std::regex::ECMAScript);
    for (auto it = std::sregex_iterator(source.begin(), source.end(), vec2_re);
         it != std::sregex_iterator();
         ++it) {
        identifiers.insert((*it)[1].str());
    }
    return identifiers;
}

[[nodiscard]] std::unordered_set<std::string> CollectScalarFunctionIdentifiers(const std::string& source)
{
    std::unordered_set<std::string> identifiers {
        "dot",
        "length",
        "distance",
    };
    const std::regex scalar_function_re(
        R"((?:^|\n)\s*(?:float|int|bool)\s+([A-Za-z_]\w*)\s*\()",
        std::regex::ECMAScript);
    for (auto it = std::sregex_iterator(source.begin(), source.end(), scalar_function_re);
         it != std::sregex_iterator();
         ++it) {
        identifiers.insert((*it)[1].str());
    }
    return identifiers;
}

[[nodiscard]] std::unordered_set<std::string> CollectTypedFunctionIdentifiers(
    const std::string& source,
    std::string_view   type_name)
{
    std::unordered_set<std::string> identifiers;
    const std::regex function_re(
        "(?:^|\\n)\\s*" + std::string(type_name) + R"(\s+([A-Za-z_]\w*)\s*\()",
        std::regex::ECMAScript);
    for (auto it = std::sregex_iterator(source.begin(), source.end(), function_re);
         it != std::sregex_iterator();
         ++it) {
        identifiers.insert((*it)[1].str());
    }
    return identifiers;
}

[[nodiscard]] std::string MaskScalarFunctionCalls(
    std::string source,
    const std::unordered_set<std::string>& scalar_functions);

[[nodiscard]] bool ContainsUnswizzledIdentifier(
    std::string_view expression,
    const std::unordered_set<std::string>& identifiers)
{
    const std::string expression_string(expression);
    for (const auto& identifier : identifiers) {
        const std::regex identifier_re(
            "\\b" + identifier + R"(\b(?!\s*\.))", std::regex::ECMAScript);
        if (std::regex_search(expression_string, identifier_re)) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::optional<int> ExtractTrailingSwizzleDimension(std::string_view expression)
{
    const std::string trimmed = trimCopy(std::string(expression));
    const wallpaper::usize dot_pos = trimmed.rfind('.');
    if (dot_pos == std::string::npos || dot_pos + 1 >= trimmed.size()) {
        return std::nullopt;
    }

    const std::string_view swizzle = std::string_view(trimmed).substr(dot_pos + 1);
    if (swizzle.empty() || swizzle.size() > 4) {
        return std::nullopt;
    }

    for (const char ch : swizzle) {
        if (std::string_view("xyzwrgba").find(ch) == std::string_view::npos) {
            return std::nullopt;
        }
    }

    return static_cast<int>(swizzle.size());
}

[[nodiscard]] std::optional<std::string> ExtractWholeFunctionName(std::string_view expression)
{
    const std::string trimmed = trimCopy(std::string(expression));
    if (trimmed.empty() || !std::isalpha(static_cast<unsigned char>(trimmed.front())) &&
                               trimmed.front() != '_') {
        return std::nullopt;
    }

    wallpaper::usize pos = 1;
    while (pos < trimmed.size() && isIdentifierChar(trimmed[pos])) {
        pos++;
    }
    if (pos >= trimmed.size() || trimmed[pos] != '(') {
        return std::nullopt;
    }

    const wallpaper::usize close_paren = FindMatchingParen(trimmed, pos);
    if (close_paren == std::string::npos || close_paren != trimmed.size() - 1) {
        return std::nullopt;
    }

    return trimmed.substr(0, pos);
}

[[nodiscard]] std::vector<std::string> SplitTopLevelBinaryOperands(std::string_view source)
{
    std::vector<std::string> operands;
    wallpaper::usize start = 0;
    int depth = 0;
    char previous_significant = '\0';

    for (wallpaper::usize pos = 0; pos < source.size(); pos++) {
        const char ch = source[pos];
        if (ch == '(') {
            depth++;
        } else if (ch == ')') {
            depth--;
        } else if (depth == 0 && (ch == '+' || ch == '-' || ch == '*' || ch == '/')) {
            const bool unary_sign =
                (ch == '+' || ch == '-') &&
                (previous_significant == '\0' ||
                 previous_significant == '(' ||
                 previous_significant == ',' ||
                 previous_significant == '+' ||
                 previous_significant == '-' ||
                 previous_significant == '*' ||
                 previous_significant == '/');
            if (!unary_sign) {
                operands.emplace_back(trimCopy(std::string(source.substr(start, pos - start))));
                start = pos + 1;
                previous_significant = ch;
                continue;
            }
        }

        if (!std::isspace(static_cast<unsigned char>(ch))) {
            previous_significant = ch;
        }
    }

    if (start == 0) {
        return {};
    }

    operands.emplace_back(trimCopy(std::string(source.substr(start))));
    return operands;
}

[[nodiscard]] std::string VectorConstructorForDimension(const int dimension)
{
    switch (dimension) {
    case 2:
        return "vec2";
    case 3:
        return "vec3";
    case 4:
        return "vec4";
    default:
        return {};
    }
}

[[nodiscard]] int InferExpressionVectorDimension(
    std::string_view expression,
    const std::unordered_set<std::string>& vec2_identifiers,
    const std::unordered_set<std::string>& vec3_identifiers,
    const std::unordered_set<std::string>& vec4_identifiers,
    const std::unordered_set<std::string>& scalar_functions,
    const std::unordered_set<std::string>& vec2_functions,
    const std::unordered_set<std::string>& vec3_functions,
    const std::unordered_set<std::string>& vec4_functions)
{
    const std::string trimmed = trimCopy(std::string(expression));
    if (const auto swizzle_dimension = ExtractTrailingSwizzleDimension(trimmed)) {
        return *swizzle_dimension;
    }

    for (const std::string_view scalar_function : { "dot", "length", "distance" }) {
        const std::string prefix = std::string(scalar_function) + "(";
        if (trimmed.starts_with(prefix)) {
            const wallpaper::usize open_paren = trimmed.find('(');
            const wallpaper::usize close_paren = FindMatchingParen(trimmed, open_paren);
            if (close_paren == trimmed.size() - 1) {
                return 1;
            }
        }
    }

    if (const auto function_name = ExtractWholeFunctionName(trimmed)) {
        if (scalar_functions.contains(*function_name)) {
            return 1;
        }
        if (vec2_functions.contains(*function_name)) {
            return 2;
        }
        if (vec3_functions.contains(*function_name)) {
            return 3;
        }
        if (vec4_functions.contains(*function_name) ||
            *function_name == "texSample2D" ||
            *function_name == "texSample2DLod" ||
            *function_name == "texture" ||
            *function_name == "textureLod") {
            return 4;
        }

        const wallpaper::usize open_paren = trimmed.find('(');
        const wallpaper::usize close_paren = FindMatchingParen(trimmed, open_paren);
        if (open_paren != std::string::npos &&
            close_paren != std::string::npos &&
            close_paren == trimmed.size() - 1) {
            const auto args = SplitTopLevelArgs(trimmed.substr(open_paren + 1, close_paren - open_paren - 1));
            int max_dim = 1;
            if (*function_name == "mix" && args.size() >= 2) {
                max_dim = std::max(
                    InferExpressionVectorDimension(
                        args[0],
                        vec2_identifiers,
                        vec3_identifiers,
                        vec4_identifiers,
                        scalar_functions,
                        vec2_functions,
                        vec3_functions,
                        vec4_functions),
                    InferExpressionVectorDimension(
                        args[1],
                        vec2_identifiers,
                        vec3_identifiers,
                        vec4_identifiers,
                        scalar_functions,
                        vec2_functions,
                        vec3_functions,
                        vec4_functions));
            } else {
                for (const auto& arg : args) {
                    max_dim = std::max(
                        max_dim,
                        InferExpressionVectorDimension(
                            arg,
                            vec2_identifiers,
                            vec3_identifiers,
                            vec4_identifiers,
                            scalar_functions,
                            vec2_functions,
                            vec3_functions,
                            vec4_functions));
                }
            }
            if (max_dim > 1) {
                return max_dim;
            }
        }
    }

    if (const auto operands = SplitTopLevelBinaryOperands(trimmed); !operands.empty()) {
        int max_dim = 1;
        for (const auto& operand : operands) {
            max_dim = std::max(
                max_dim,
                InferExpressionVectorDimension(
                    operand,
                    vec2_identifiers,
                    vec3_identifiers,
                    vec4_identifiers,
                    scalar_functions,
                    vec2_functions,
                    vec3_functions,
                    vec4_functions));
        }
        if (max_dim > 1) {
            return max_dim;
        }
    }

    const std::string masked_expression =
        MaskScalarFunctionCalls(std::string(expression), scalar_functions);

    static const std::regex scalar_texture_sample_re(
        R"((?:texSample2D|texSample2DLod|texture|textureLod)\s*\(.*\)\s*\.[xyzwrgba]\b)",
        std::regex::ECMAScript);
    if (std::regex_search(masked_expression, scalar_texture_sample_re)) {
        return 1;
    }

    if (masked_expression.find("vec4(") != std::string::npos ||
        masked_expression.find("CAST4(") != std::string::npos ||
        vec4_identifiers.contains(trimmed)) {
        return 4;
    }
    if (masked_expression.find("vec3(") != std::string::npos ||
        masked_expression.find("CAST3(") != std::string::npos ||
        vec3_identifiers.contains(trimmed)) {
        return 3;
    }
    if (masked_expression.find("vec2(") != std::string::npos ||
        masked_expression.find("CAST2(") != std::string::npos ||
        vec2_identifiers.contains(trimmed)) {
        return 2;
    }
    return 1;
}

[[nodiscard]] wallpaper::usize FindTopLevelComma(std::string_view source)
{
    int depth = 0;
    for (wallpaper::usize pos = 0; pos < source.size(); pos++) {
        if (source[pos] == '(') {
            depth++;
        } else if (source[pos] == ')') {
            depth--;
        } else if (source[pos] == ',' && depth == 0) {
            return pos;
        }
    }
    return std::string::npos;
}

[[nodiscard]] std::optional<std::string> ExtractScalarBroadcast(std::string_view expression)
{
    const std::string trimmed = trimCopy(std::string(expression));
    std::smatch match;
    const std::regex broadcast_re(
        R"(^(?:vec2|vec3|CAST2|CAST3)\(\s*([^,()]+)\s*\)$)",
        std::regex::ECMAScript);
    if (std::regex_match(trimmed, match, broadcast_re)) {
        return trimCopy(match[1].str());
    }
    return std::nullopt;
}

[[nodiscard]] std::string MaskScalarFunctionCalls(
    std::string source,
    const std::unordered_set<std::string>& scalar_functions)
{
    const std::regex function_call_re(R"(\b([A-Za-z_]\w*)\s*\()", std::regex::ECMAScript);
    wallpaper::usize search_pos = 0;

    while (search_pos < source.size()) {
        std::smatch match;
        auto begin = source.cbegin() + static_cast<wallpaper::isize>(search_pos);
        if (!std::regex_search(begin, source.cend(), match, function_call_re)) {
            break;
        }

        const wallpaper::usize match_pos = search_pos + static_cast<wallpaper::usize>(match.position(0));
        const wallpaper::usize name_pos = search_pos + static_cast<wallpaper::usize>(match.position(1));
        const std::string function_name = match[1].str();
        const wallpaper::usize open_paren = source.find('(', match_pos);
        if (open_paren == std::string::npos) {
            break;
        }

        if (scalar_functions.count(function_name) == 0) {
            search_pos = open_paren + 1;
            continue;
        }

        const wallpaper::usize close_paren = FindMatchingParen(source, open_paren);
        if (close_paren == std::string::npos) {
            break;
        }

        source.replace(name_pos, close_paren - name_pos + 1, "0.0");
        search_pos = name_pos + 3;
    }

    return source;
}

[[nodiscard]] std::string RewriteVec3Vec2BinaryOps(std::string source)
{
    const auto vec3_identifiers = CollectVec3Identifiers(source);
    if (vec3_identifiers.empty()) {
        return source;
    }

    const std::regex rhs_vec2_re(
        R"((\b[A-Za-z_]\w*\b)\s*([+\-])\s*\(?(?:vec2|CAST2)\()",
        std::regex::ECMAScript);
    wallpaper::usize search_pos = 0;
    while (search_pos < source.size()) {
        const wallpaper::usize line_end = source.find('\n', search_pos);
        const wallpaper::usize slice_end = line_end == std::string::npos ? source.size() : line_end;
        std::string line = source.substr(search_pos, slice_end - search_pos);
        std::smatch match;
        if (std::regex_search(line, match, rhs_vec2_re)) {
            const std::string identifier = match[1].str();
            if (vec3_identifiers.count(identifier) != 0) {
                wallpaper::usize vec2_pos = line.find("vec2(", static_cast<wallpaper::usize>(match.position()));
                if (vec2_pos == std::string::npos) {
                    vec2_pos = line.find("CAST2(", static_cast<wallpaper::usize>(match.position()));
                }
                const wallpaper::usize open_paren = line.find('(', vec2_pos);
                const wallpaper::usize close_paren = FindMatchingParen(line, open_paren);
                if (vec2_pos != std::string::npos && close_paren != std::string::npos) {
                    const std::string operand = line.substr(vec2_pos, close_paren - vec2_pos + 1);
                    line.replace(
                        vec2_pos,
                        close_paren - vec2_pos + 1,
                        "vec3(" + operand + ", 0.0)");
                    source.replace(search_pos, slice_end - search_pos, line);
                    search_pos += line.size();
                    continue;
                }
            }
        }
        search_pos = line_end == std::string::npos ? source.size() : line_end + 1;
    }

    return source;
}

[[nodiscard]] std::unordered_set<std::string> CollectBoolIdentifiers(const std::string& source)
{
    std::unordered_set<std::string> identifiers;
    const std::regex bool_re(R"(^\s*bool\s+([A-Za-z_]\w*)\s*=)", std::regex::ECMAScript);

    wallpaper::usize search_pos = 0;
    while (search_pos < source.size()) {
        const wallpaper::usize line_end = source.find('\n', search_pos);
        const wallpaper::usize slice_end = line_end == std::string::npos ? source.size() : line_end;
        const std::string line = source.substr(search_pos, slice_end - search_pos);
        std::smatch match;
        if (std::regex_search(line, match, bool_re)) {
            identifiers.insert(match[1].str());
        }
        search_pos = line_end == std::string::npos ? source.size() : line_end + 1;
    }

    return identifiers;
}

[[nodiscard]] std::vector<std::string> SplitTopLevelArgs(std::string_view source)
{
    std::vector<std::string> args;
    wallpaper::usize start = 0;
    int depth = 0;
    for (wallpaper::usize pos = 0; pos < source.size(); pos++) {
        if (source[pos] == '(') {
            depth++;
        } else if (source[pos] == ')') {
            depth--;
        } else if (source[pos] == ',' && depth == 0) {
            args.emplace_back(trimCopy(std::string(source.substr(start, pos - start))));
            start = pos + 1;
        }
    }
    args.emplace_back(trimCopy(std::string(source.substr(start))));
    return args;
}

[[nodiscard]] bool IsPureIntegerLiteral(std::string_view source)
{
    static const std::regex integer_re(R"(^[-+]?[0-9]+$)", std::regex::ECMAScript);
    return std::regex_match(trimCopy(std::string(source)), integer_re);
}

[[nodiscard]] bool IsSimpleNumericMacroOrLiteral(
    std::string_view condition,
    const std::map<std::string, int64_t>& macros)
{
    std::string trimmed = trimCopy(std::string(condition));

    while (trimmed.size() >= 2 && trimmed.front() == '(' && trimmed.back() == ')') {
        const wallpaper::usize close_paren = FindMatchingParen(trimmed, 0);
        if (close_paren != trimmed.size() - 1) {
            break;
        }
        trimmed = trimCopy(trimmed.substr(1, trimmed.size() - 2));
    }

    if (trimmed.empty()) {
        return false;
    }

    return IsPureIntegerLiteral(trimmed) || macros.contains(trimmed);
}

[[nodiscard]] bool IsAssignmentDelimiter(std::string_view line, const wallpaper::usize pos)
{
    if (pos >= line.size() || line[pos] != '=') {
        return false;
    }

    if (pos > 0 && (line[pos - 1] == '=' || line[pos - 1] == '!' ||
                    line[pos - 1] == '<' || line[pos - 1] == '>')) {
        return false;
    }

    return pos + 1 >= line.size() || line[pos + 1] != '=';
}

[[nodiscard]] wallpaper::usize FindTernaryConditionStart(
    std::string_view line,
    const wallpaper::usize question_pos)
{
    int paren_depth = 0;
    int bracket_depth = 0;
    int brace_depth = 0;

    for (wallpaper::usize pos = question_pos; pos > 0;) {
        --pos;
        const char ch = line[pos];

        if (ch == ')') {
            paren_depth++;
            continue;
        }
        if (ch == ']') {
            bracket_depth++;
            continue;
        }
        if (ch == '}') {
            brace_depth++;
            continue;
        }
        if (ch == '(') {
            if (paren_depth == 0 && bracket_depth == 0 && brace_depth == 0) {
                return pos + 1;
            }
            paren_depth = std::max(0, paren_depth - 1);
            continue;
        }
        if (ch == '[') {
            bracket_depth = std::max(0, bracket_depth - 1);
            continue;
        }
        if (ch == '{') {
            if (paren_depth == 0 && bracket_depth == 0 && brace_depth == 0) {
                return pos + 1;
            }
            brace_depth = std::max(0, brace_depth - 1);
            continue;
        }

        if (paren_depth != 0 || bracket_depth != 0 || brace_depth != 0) {
            continue;
        }

        if (ch == ',' || ch == ';' || IsAssignmentDelimiter(line, pos)) {
            return pos + 1;
        }
    }

    return 0;
}

[[nodiscard]] std::string RewriteNumericMacroTernaryConditions(
    std::string source,
    const std::map<std::string, int64_t>& macros)
{
    wallpaper::usize search_pos = 0;
    while (search_pos < source.size()) {
        const wallpaper::usize line_end = source.find('\n', search_pos);
        const wallpaper::usize slice_end = line_end == std::string::npos ? source.size() : line_end;
        std::string line = source.substr(search_pos, slice_end - search_pos);
        const std::string trimmed = trimCopy(line);

        if (startsWith(trimmed, "#")) {
            search_pos = line_end == std::string::npos ? source.size() : line_end + 1;
            continue;
        }

        wallpaper::usize code_end = line.find("//");
        if (code_end == std::string::npos) {
            code_end = line.size();
        }

        wallpaper::usize question_pos = 0;
        while ((question_pos = line.find('?', question_pos)) != std::string::npos &&
               question_pos < code_end) {
            wallpaper::usize condition_start = FindTernaryConditionStart(line, question_pos);
            while (condition_start < question_pos &&
                   std::isspace(static_cast<unsigned char>(line[condition_start]))) {
                condition_start++;
            }

            wallpaper::usize condition_end = question_pos;
            while (condition_end > condition_start &&
                   std::isspace(static_cast<unsigned char>(line[condition_end - 1]))) {
                condition_end--;
            }

            if (condition_start >= condition_end) {
                question_pos++;
                continue;
            }

            const std::string condition =
                line.substr(condition_start, condition_end - condition_start);
            if (!IsSimpleNumericMacroOrLiteral(condition, macros)) {
                question_pos++;
                continue;
            }

            const std::string replacement = "(" + condition + " != 0)";
            line.replace(condition_start, condition.size(), replacement);
            const wallpaper::usize delta = replacement.size() - condition.size();
            code_end += delta;
            question_pos = condition_start + replacement.size() + 1;
        }

        source.replace(search_pos, slice_end - search_pos, line);
        search_pos = search_pos + line.size() + (line_end == std::string::npos ? 0 : 1);
    }

    return source;
}

[[nodiscard]] std::string RewriteBuiltinIntegerLiteralArguments(std::string source)
{
    const std::regex builtin_call_re(R"(\b(mix|smoothstep|step|pow|clamp)\s*\()", std::regex::ECMAScript);
    wallpaper::usize search_pos = 0;

    while (search_pos < source.size()) {
        std::smatch match;
        auto begin = source.cbegin() + static_cast<wallpaper::isize>(search_pos);
        if (!std::regex_search(begin, source.cend(), match, builtin_call_re)) {
            break;
        }

        const wallpaper::usize match_pos = search_pos + static_cast<wallpaper::usize>(match.position(0));
        const wallpaper::usize name_pos = search_pos + static_cast<wallpaper::usize>(match.position(1));
        const std::string function_name = match[1].str();
        const wallpaper::usize open_paren = source.find('(', match_pos);
        if (open_paren == std::string::npos) {
            break;
        }

        const wallpaper::usize close_paren = FindMatchingParen(source, open_paren);
        if (close_paren == std::string::npos) {
            break;
        }

        const std::string args_source = source.substr(open_paren + 1, close_paren - open_paren - 1);
        auto args = SplitTopLevelArgs(args_source);
        bool changed = false;
        for (auto& arg : args) {
            if (IsPureIntegerLiteral(arg)) {
                arg += ".0";
                changed = true;
            }
        }

        if (!changed) {
            search_pos = close_paren + 1;
            continue;
        }

        std::ostringstream rebuilt;
        rebuilt << function_name << '(';
        for (wallpaper::usize index = 0; index < args.size(); index++) {
            if (index != 0) rebuilt << ", ";
            rebuilt << args[index];
        }
        rebuilt << ')';

        source.replace(name_pos, close_paren - name_pos + 1, rebuilt.str());
        search_pos = name_pos + rebuilt.str().size();
    }

    return source;
}

[[nodiscard]] std::string RewriteBuiltinVectorScalarArguments(std::string source)
{
    const auto vec2_identifiers = CollectVec2Identifiers(source);
    const auto vec3_identifiers = CollectVec3Identifiers(source);
    const auto vec4_identifiers = CollectVec4Identifiers(source);
    const auto scalar_functions = CollectScalarFunctionIdentifiers(source);
    const auto vec2_functions = CollectTypedFunctionIdentifiers(source, "vec2");
    const auto vec3_functions = CollectTypedFunctionIdentifiers(source, "vec3");
    const auto vec4_functions = CollectTypedFunctionIdentifiers(source, "vec4");
    const std::regex builtin_call_re(R"(\b(mix|smoothstep|step|pow|clamp)\s*\()", std::regex::ECMAScript);
    wallpaper::usize search_pos = 0;

    while (search_pos < source.size()) {
        std::smatch match;
        auto begin = source.cbegin() + static_cast<wallpaper::isize>(search_pos);
        if (!std::regex_search(begin, source.cend(), match, builtin_call_re)) {
            break;
        }

        const wallpaper::usize match_pos = search_pos + static_cast<wallpaper::usize>(match.position(0));
        const wallpaper::usize name_pos = search_pos + static_cast<wallpaper::usize>(match.position(1));
        const std::string function_name = match[1].str();
        const wallpaper::usize open_paren = source.find('(', match_pos);
        if (open_paren == std::string::npos) {
            break;
        }

        const wallpaper::usize close_paren = FindMatchingParen(source, open_paren);
        if (close_paren == std::string::npos) {
            break;
        }

        const std::string args_source = source.substr(open_paren + 1, close_paren - open_paren - 1);
        auto args = SplitTopLevelArgs(args_source);
        int max_dim = 1;
        std::vector<int> dims;
        dims.reserve(args.size());
        for (const auto& arg : args) {
            const int dim = InferExpressionVectorDimension(
                arg,
                vec2_identifiers,
                vec3_identifiers,
                vec4_identifiers,
                scalar_functions,
                vec2_functions,
                vec3_functions,
                vec4_functions);
            dims.push_back(dim);
            max_dim = std::max(max_dim, dim);
        }

        if (max_dim == 1) {
            search_pos = close_paren + 1;
            continue;
        }

        bool changed = false;
        if (function_name == "mix" && args.size() == 3) {
            const int value_dim = std::max(dims[0], dims[1]);
            const std::string vector_ctor = VectorConstructorForDimension(value_dim);
            if (!vector_ctor.empty()) {
                for (wallpaper::usize index : { 0u, 1u }) {
                    if (dims[index] == 1) {
                        args[index] = vector_ctor + "(" + trimCopy(args[index]) + ")";
                        changed = true;
                    }
                }
                if (dims[2] != 1 && dims[2] != value_dim) {
                    args[2] = vector_ctor + "(" + trimCopy(args[2]) + ")";
                    changed = true;
                }
            }
        } else if (function_name == "pow" && args.size() == 2) {
            const std::string vector_ctor = VectorConstructorForDimension(max_dim);
            if (!vector_ctor.empty()) {
                for (wallpaper::usize index : { 0u, 1u }) {
                    if (dims[index] == 1) {
                        args[index] = vector_ctor + "(" + trimCopy(args[index]) + ")";
                        changed = true;
                    }
                }
            }
        } else if (function_name == "clamp" && args.size() == 3) {
            const std::string vector_ctor = VectorConstructorForDimension(max_dim);
            if (!vector_ctor.empty()) {
                for (wallpaper::usize index : { 0u, 1u, 2u }) {
                    if (dims[index] == 1) {
                        args[index] = vector_ctor + "(" + trimCopy(args[index]) + ")";
                        changed = true;
                    }
                }
            }
        } else {
            const std::string vector_ctor = VectorConstructorForDimension(max_dim);
            if (!vector_ctor.empty()) {
                for (wallpaper::usize index = 0; index < args.size(); index++) {
                    if (dims[index] == 1) {
                        args[index] = vector_ctor + "(" + trimCopy(args[index]) + ")";
                        changed = true;
                    }
                }
            }
        }

        if (!changed) {
            search_pos = close_paren + 1;
            continue;
        }

        std::ostringstream rebuilt;
        rebuilt << function_name << '(';
        for (wallpaper::usize index = 0; index < args.size(); index++) {
            if (index != 0) rebuilt << ", ";
            rebuilt << args[index];
        }
        rebuilt << ')';

        source.replace(name_pos, close_paren - name_pos + 1, rebuilt.str());
        search_pos = name_pos + rebuilt.str().size();
    }

    return source;
}

[[nodiscard]] std::string RewriteBoolArithmetic(std::string source)
{
    const auto bool_identifiers = CollectBoolIdentifiers(source);
    if (bool_identifiers.empty()) {
        return source;
    }
    wallpaper::usize search_pos = 0;
    while ((search_pos = source.find("*=", search_pos)) != std::string::npos) {
        wallpaper::usize lhs_end = search_pos;
        while (lhs_end > 0 && std::isspace(static_cast<unsigned char>(source[lhs_end - 1]))) lhs_end--;
        wallpaper::usize lhs_start = lhs_end;
        while (lhs_start > 0 &&
               (isIdentifierChar(source[lhs_start - 1]) || source[lhs_start - 1] == '.')) {
            lhs_start--;
        }

        wallpaper::usize rhs_start = search_pos + 2;
        while (rhs_start < source.size() &&
               std::isspace(static_cast<unsigned char>(source[rhs_start]))) {
            rhs_start++;
        }
        wallpaper::usize rhs_end = rhs_start;
        while (rhs_end < source.size() && isIdentifierChar(source[rhs_end])) rhs_end++;

        const std::string rhs = source.substr(rhs_start, rhs_end - rhs_start);
        if (!bool_identifiers.contains(rhs)) {
            search_pos = rhs_end;
            continue;
        }

        const std::string lhs = trimCopy(source.substr(lhs_start, lhs_end - lhs_start));
        const std::string rewritten = lhs + " *= (" + rhs + " ? 1.0 : 0.0)";
        source.replace(lhs_start, rhs_end - lhs_start, rewritten);
        search_pos = lhs_start + rewritten.size();
    }

    return source;
}

[[nodiscard]] std::unordered_set<std::string> CollectStageInterfaceIdentifiers(const std::string& source)
{
    std::unordered_set<std::string> identifiers;
    static constexpr std::array<std::string_view, 5> qualifiers {
        "varying ",
        "uniform ",
        "attribute ",
        "in ",
        "out ",
    };

    wallpaper::usize search_pos = 0;
    while (search_pos < source.size()) {
        const wallpaper::usize line_end = source.find('\n', search_pos);
        const wallpaper::usize slice_end = line_end == std::string::npos ? source.size() : line_end;
        const std::string line = trimCopy(source.substr(search_pos, slice_end - search_pos));
        for (const auto qualifier : qualifiers) {
            if (!startsWith(line, qualifier)) {
                continue;
            }

            wallpaper::usize pos = qualifier.size();
            while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) pos++;
            while (pos < line.size() && isIdentifierChar(line[pos])) pos++;
            while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) pos++;
            const wallpaper::usize begin = pos;
            while (pos < line.size() && isIdentifierChar(line[pos])) pos++;
            if (pos > begin) {
                identifiers.insert(line.substr(begin, pos - begin));
            }
            break;
        }
        search_pos = line_end == std::string::npos ? source.size() : line_end + 1;
    }

    return identifiers;
}

[[nodiscard]] std::string RewriteStageIdentifierCollisions(std::string source)
{
    const auto interface_identifiers = CollectStageInterfaceIdentifiers(source);
    auto declared_identifiers = interface_identifiers;
    static const std::unordered_set<std::string> reserved_identifiers {
        "and",
        "or",
        "xor",
        "not",
    };
    static constexpr std::array<std::string_view, 6> local_types {
        "float ",
        "vec2 ",
        "vec3 ",
        "vec4 ",
        "int ",
        "uint ",
    };

    const auto replace_identifier_occurrences =
        [](std::string_view input, std::string_view needle, std::string_view replacement) {
            std::string output;
            output.reserve(input.size());
            wallpaper::usize pos = 0;
            while (pos < input.size()) {
                const wallpaper::usize found = input.find(needle, pos);
                if (found == std::string::npos) {
                    output.append(input.substr(pos));
                    break;
                }

                const bool left_ok =
                    found == 0 || !isIdentifierChar(input[found - 1]);
                const wallpaper::usize after = found + needle.size();
                const bool right_ok =
                    after >= input.size() || !isIdentifierChar(input[after]);
                if (left_ok && right_ok) {
                    output.append(input.substr(pos, found - pos));
                    output.append(replacement);
                    pos = after;
                } else {
                    output.append(input.substr(pos, found - pos + needle.size()));
                    pos = after;
                }
            }
            return output;
        };

    wallpaper::usize search_pos = 0;
    while (search_pos < source.size()) {
        const wallpaper::usize line_end = source.find('\n', search_pos);
        const wallpaper::usize slice_end = line_end == std::string::npos ? source.size() : line_end;
        std::string line = source.substr(search_pos, slice_end - search_pos);
        std::string trimmed = trimCopy(line);
        if (startsWith(trimmed, "const ")) {
            trimmed = trimCopy(trimmed.substr(6));
        }

        for (const auto type_prefix : local_types) {
            if (!startsWith(trimmed, type_prefix)) {
                continue;
            }

            wallpaper::usize pos = type_prefix.size();
            const wallpaper::usize begin = pos;
            while (pos < trimmed.size() && isIdentifierChar(trimmed[pos])) pos++;
            if (pos == begin) break;

            const std::string identifier = trimmed.substr(begin, pos - begin);
            if (trimmed.find('=', pos) == std::string::npos) break;
            if (!interface_identifiers.contains(identifier) &&
                !declared_identifiers.contains(identifier) &&
                !reserved_identifiers.contains(identifier)) {
                declared_identifiers.insert(identifier);
                break;
            }

            const std::string renamed = identifier + "_local";
            const wallpaper::usize identifier_pos = line.find(identifier);
            if (identifier_pos == std::string::npos) break;

            line.replace(identifier_pos, identifier.size(), renamed);
            const std::string tail =
                replace_identifier_occurrences(source.substr(slice_end), identifier, renamed);

            source = source.substr(0, search_pos) + line + tail;
            declared_identifiers.insert(renamed);
            search_pos += line.size();
            goto next_line;
        }

        search_pos = line_end == std::string::npos ? source.size() : line_end + 1;
    next_line:
        continue;
    }

    return source;
}

[[nodiscard]] std::string RewriteVectorWidthInitializers(std::string source)
{
    const auto vec4_identifiers = CollectVec4Identifiers(source);
    if (vec4_identifiers.empty()) {
        return source;
    }

    const std::regex vector_init_re(R"(^\s*(vec[23])\s+[A-Za-z_]\w*\s*=\s*(.+);\r?$)");

    wallpaper::usize search_pos = 0;
    while (search_pos < source.size()) {
        const wallpaper::usize line_end = source.find('\n', search_pos);
        const wallpaper::usize slice_end = line_end == std::string::npos ? source.size() : line_end;
        std::string line = source.substr(search_pos, slice_end - search_pos);
        std::smatch match;

        if (std::regex_match(line, match, vector_init_re)) {
            const std::string target_type = match[1].str();
            std::string rhs = match[2].str();
            const std::string swizzle = target_type == "vec2" ? ".xy" : ".xyz";
            bool changed = false;

            for (const auto& identifier : vec4_identifiers) {
                const std::regex identifier_re(
                    "\\b" + identifier + R"(\b(?!\s*\.))",
                    std::regex::ECMAScript);
                std::string rewritten_rhs = std::regex_replace(rhs, identifier_re, identifier + swizzle);
                if (rewritten_rhs != rhs) {
                    rhs = std::move(rewritten_rhs);
                    changed = true;
                }
            }

            if (changed) {
                const wallpaper::usize equal_pos = line.find('=');
                line.replace(equal_pos + 1, std::string::npos, " " + rhs + ";");
                source.replace(search_pos, slice_end - search_pos, line);
                search_pos += line.size();
                continue;
            }
        }

        search_pos = line_end == std::string::npos ? source.size() : line_end + 1;
    }

    return source;
}

[[nodiscard]] int InferAssignmentTargetDimension(
    std::string_view declared_type,
    std::string_view lhs,
    const std::unordered_set<std::string>& vec2_identifiers,
    const std::unordered_set<std::string>& vec3_identifiers)
{
    if (declared_type == "vec2") {
        return 2;
    }
    if (declared_type == "vec3") {
        return 3;
    }

    const wallpaper::usize dot_pos = lhs.find('.');
    if (dot_pos != std::string::npos && dot_pos + 1 < lhs.size()) {
        const std::string_view swizzle = lhs.substr(dot_pos + 1);
        if (swizzle.size() == 2 || swizzle.size() == 3) {
            return static_cast<int>(swizzle.size());
        }
    }

    if (vec2_identifiers.contains(std::string(lhs))) {
        return 2;
    }
    if (vec3_identifiers.contains(std::string(lhs))) {
        return 3;
    }

    return 0;
}

[[nodiscard]] std::string RewriteVectorWidthBuiltinAssignments(std::string source)
{
    const auto vec2_identifiers = CollectVec2Identifiers(source);
    const auto vec3_identifiers = CollectVec3Identifiers(source);
    const auto vec4_identifiers = CollectVec4Identifiers(source);
    if (vec4_identifiers.empty()) {
        return source;
    }

    const std::regex assignment_re(
        R"(^\s*(?:(vec[234])\s+)?([A-Za-z_]\w*(?:\.[xyzwrgba]{1,4})?)\s*=\s*(.+);\r?$)");

    wallpaper::usize search_pos = 0;
    while (search_pos < source.size()) {
        const wallpaper::usize line_end = source.find('\n', search_pos);
        const wallpaper::usize slice_end = line_end == std::string::npos ? source.size() : line_end;
        std::string line = source.substr(search_pos, slice_end - search_pos);
        std::smatch match;

        if (std::regex_match(line, match, assignment_re)) {
            const std::string declared_type = match[1].matched ? match[1].str() : "";
            const std::string lhs = match[2].str();
            std::string rhs = match[3].str();
            const int target_dim =
                InferAssignmentTargetDimension(declared_type, lhs, vec2_identifiers, vec3_identifiers);

            if ((target_dim == 2 || target_dim == 3) &&
                (rhs.find("mix(") != std::string::npos || rhs.find("smoothstep(") != std::string::npos)) {
                const std::string swizzle = target_dim == 2 ? ".xy" : ".xyz";
                bool changed = false;

                for (const auto& identifier : vec4_identifiers) {
                    const std::regex identifier_re(
                        "\\b" + identifier + R"(\b(?!\s*\.))",
                        std::regex::ECMAScript);
                    std::string rewritten_rhs =
                        std::regex_replace(rhs, identifier_re, identifier + swizzle);
                    if (rewritten_rhs != rhs) {
                        rhs = std::move(rewritten_rhs);
                        changed = true;
                    }
                }

                if (changed) {
                    const wallpaper::usize equal_pos = line.find('=');
                    line.replace(equal_pos + 1, std::string::npos, " " + rhs + ";");
                    source.replace(search_pos, slice_end - search_pos, line);
                    search_pos += line.size();
                    continue;
                }
            }
        }

        search_pos = line_end == std::string::npos ? source.size() : line_end + 1;
    }

    return source;
}

[[nodiscard]] std::string RewriteVectorScalarMaxCalls(std::string source)
{
    const auto vec2_identifiers = CollectVec2Identifiers(source);
    const auto vec3_identifiers = CollectVec3Identifiers(source);
    const auto vec4_identifiers = CollectVec4Identifiers(source);
    const auto scalar_functions = CollectScalarFunctionIdentifiers(source);
    const auto vec2_functions = CollectTypedFunctionIdentifiers(source, "vec2");
    const auto vec3_functions = CollectTypedFunctionIdentifiers(source, "vec3");
    const auto vec4_functions = CollectTypedFunctionIdentifiers(source, "vec4");

    wallpaper::usize search_pos = 0;
    while ((search_pos = source.find("max(", search_pos)) != std::string::npos) {
        const wallpaper::usize open_paren = source.find('(', search_pos);
        if (open_paren == std::string::npos) {
            break;
        }
        const wallpaper::usize close_paren = FindMatchingParen(source, open_paren);
        if (close_paren == std::string::npos) {
            break;
        }

        const std::string args = source.substr(open_paren + 1, close_paren - open_paren - 1);
        const wallpaper::usize comma_pos = FindTopLevelComma(args);
        if (comma_pos == std::string::npos) {
            search_pos = close_paren + 1;
            continue;
        }

        const std::string lhs = trimCopy(args.substr(0, comma_pos));
        const std::string rhs = trimCopy(args.substr(comma_pos + 1));
        const int lhs_dim = InferExpressionVectorDimension(
            lhs,
            vec2_identifiers,
            vec3_identifiers,
            vec4_identifiers,
            scalar_functions,
            vec2_functions,
            vec3_functions,
            vec4_functions);
        const int rhs_dim = InferExpressionVectorDimension(
            rhs,
            vec2_identifiers,
            vec3_identifiers,
            vec4_identifiers,
            scalar_functions,
            vec2_functions,
            vec3_functions,
            vec4_functions);
        const auto lhs_scalar_broadcast = ExtractScalarBroadcast(lhs);
        const auto rhs_scalar_broadcast = ExtractScalarBroadcast(rhs);

        if (lhs_scalar_broadcast.has_value() && rhs_dim == 1) {
            const std::string rewritten = "max(" + *lhs_scalar_broadcast + ", " + rhs + ")";
            source.replace(search_pos, close_paren - search_pos + 1, rewritten);
            search_pos += rewritten.size();
            continue;
        }

        if (rhs_scalar_broadcast.has_value() && lhs_dim == 1) {
            const std::string rewritten = "max(" + lhs + ", " + *rhs_scalar_broadcast + ")";
            source.replace(search_pos, close_paren - search_pos + 1, rewritten);
            search_pos += rewritten.size();
            continue;
        }

        if (lhs_dim == rhs_dim || (lhs_dim != 1 && rhs_dim != 1)) {
            search_pos = close_paren + 1;
            continue;
        }

        const int vector_dim = lhs_dim == 1 ? rhs_dim : lhs_dim;
        const std::string vector_ctor = vector_dim == 3 ? "vec3" : "vec2";
        const std::string rewritten =
            lhs_dim == 1
                ? "max(" + vector_ctor + "(" + lhs + "), " + rhs + ")"
                : "max(" + lhs + ", " + vector_ctor + "(" + rhs + "))";

        source.replace(search_pos, close_paren - search_pos + 1, rewritten);
        search_pos += rewritten.size();
    }

    return source;
}

[[nodiscard]] std::string RewriteFloatVectorInitializers(std::string source)
{
    const auto vec2_identifiers = CollectVec2Identifiers(source);
    const auto vec3_identifiers = CollectVec3Identifiers(source);
    const auto scalar_functions = CollectScalarFunctionIdentifiers(source);
    if (vec2_identifiers.empty() && vec3_identifiers.empty()) {
        return source;
    }

    const std::regex float_init_re(R"(^\s*float\s+[A-Za-z_]\w*\s*=\s*(.+);\r?$)");

    wallpaper::usize search_pos = 0;
    while (search_pos < source.size()) {
        const wallpaper::usize line_end = source.find('\n', search_pos);
        const wallpaper::usize slice_end = line_end == std::string::npos ? source.size() : line_end;
        std::string line = source.substr(search_pos, slice_end - search_pos);
        std::smatch match;
        if (std::regex_match(line, match, float_init_re)) {
            const std::string rhs = match[1].str();
            const std::string trimmed_rhs = trimCopy(rhs);
            if (trimmed_rhs.starts_with("max(")) {
                const wallpaper::usize open_paren = trimmed_rhs.find('(');
                const wallpaper::usize close_paren = FindMatchingParen(trimmed_rhs, open_paren);
                if (close_paren == trimmed_rhs.size() - 1) {
                    const std::string args =
                        trimmed_rhs.substr(open_paren + 1, close_paren - open_paren - 1);
                    const wallpaper::usize comma_pos = FindTopLevelComma(args);
                    if (comma_pos != std::string::npos) {
                        const std::string lhs = trimCopy(args.substr(0, comma_pos));
                        const std::string rhs_arg = trimCopy(args.substr(comma_pos + 1));
                        if (const auto lhs_scalar = ExtractScalarBroadcast(lhs)) {
                            const wallpaper::usize equal_pos = line.find('=');
                            line.replace(
                                equal_pos + 1,
                                std::string::npos,
                                " max(" + *lhs_scalar + ", " + rhs_arg + ");");
                            source.replace(search_pos, slice_end - search_pos, line);
                            search_pos += line.size();
                            continue;
                        }
                        if (const auto rhs_scalar = ExtractScalarBroadcast(rhs_arg)) {
                            const wallpaper::usize equal_pos = line.find('=');
                            line.replace(
                                equal_pos + 1,
                                std::string::npos,
                                " max(" + lhs + ", " + *rhs_scalar + ");");
                            source.replace(search_pos, slice_end - search_pos, line);
                            search_pos += line.size();
                            continue;
                        }
                    }
                }
            }
            const std::string masked_rhs = MaskScalarFunctionCalls(rhs, scalar_functions);
            const bool has_scalar_swizzle =
                std::regex_search(masked_rhs, std::regex(R"(\.\s*[xyzwrgba]\b)", std::regex::ECMAScript));
            if (!has_scalar_swizzle && masked_rhs.find("dot(") == std::string::npos &&
                masked_rhs.find("length(") == std::string::npos) {
                const bool uses_vec3 = ContainsUnswizzledIdentifier(masked_rhs, vec3_identifiers);

                bool uses_vec2 = masked_rhs.find("vec2(") != std::string::npos ||
                                 masked_rhs.find("CAST2(") != std::string::npos;
                if (!uses_vec2) {
                    uses_vec2 = ContainsUnswizzledIdentifier(masked_rhs, vec2_identifiers);
                }

                if (uses_vec3 || uses_vec2) {
                    std::string scalarized_rhs;
                    if (uses_vec3) {
                        scalarized_rhs =
                            "max(max((" + rhs + ").x, (" + rhs + ").y), (" + rhs + ").z)";
                    } else {
                        scalarized_rhs = "max((" + rhs + ").x, (" + rhs + ").y)";
                    }

                    const wallpaper::usize equal_pos = line.find('=');
                    line.replace(equal_pos + 1, std::string::npos, " " + scalarized_rhs + ";");
                    source.replace(search_pos, slice_end - search_pos, line);
                    search_pos += line.size();
                    continue;
                }
            }
        }

        search_pos = line_end == std::string::npos ? source.size() : line_end + 1;
    }

    return source;
}

[[nodiscard]] std::string RewriteFloatDeclarationsToVectors(std::string source)
{
    const auto vec2_identifiers = CollectVec2Identifiers(source);
    const auto vec3_identifiers = CollectVec3Identifiers(source);
    const auto vec4_identifiers = CollectVec4Identifiers(source);
    const auto scalar_functions = CollectScalarFunctionIdentifiers(source);
    const auto vec2_functions = CollectTypedFunctionIdentifiers(source, "vec2");
    const auto vec3_functions = CollectTypedFunctionIdentifiers(source, "vec3");
    const auto vec4_functions = CollectTypedFunctionIdentifiers(source, "vec4");
    const std::regex float_init_re(R"(^\s*float\s+([A-Za-z_]\w*)\s*=\s*(.+);\r?$)");

    wallpaper::usize search_pos = 0;
    while (search_pos < source.size()) {
        const wallpaper::usize line_end = source.find('\n', search_pos);
        const wallpaper::usize slice_end = line_end == std::string::npos ? source.size() : line_end;
        std::string line = source.substr(search_pos, slice_end - search_pos);
        std::smatch match;

        if (std::regex_match(line, match, float_init_re)) {
            const std::string rhs = match[2].str();
            const std::string masked_rhs = MaskScalarFunctionCalls(rhs, scalar_functions);
            const int rhs_dim = InferExpressionVectorDimension(
                rhs,
                vec2_identifiers,
                vec3_identifiers,
                vec4_identifiers,
                scalar_functions,
                vec2_functions,
                vec3_functions,
                vec4_functions);

            const bool explicit_vec2 =
                masked_rhs.find(".xy") != std::string::npos ||
                masked_rhs.find("vec2(") != std::string::npos ||
                masked_rhs.find("CAST2(") != std::string::npos ||
                ContainsUnswizzledIdentifier(masked_rhs, vec2_identifiers);
            const bool explicit_vec3 =
                masked_rhs.find(".xyz") != std::string::npos ||
                masked_rhs.find("vec3(") != std::string::npos ||
                masked_rhs.find("CAST3(") != std::string::npos ||
                ContainsUnswizzledIdentifier(masked_rhs, vec3_identifiers);

            if ((rhs_dim == 2 && explicit_vec2) || (rhs_dim == 3 && explicit_vec3)) {
                const wallpaper::usize float_pos = line.find("float");
                if (float_pos != std::string::npos) {
                    line.replace(float_pos, 5, rhs_dim == 3 ? "vec3" : "vec2");
                    source.replace(search_pos, slice_end - search_pos, line);
                    search_pos += line.size();
                    continue;
                }
            }
        }

        search_pos = line_end == std::string::npos ? source.size() : line_end + 1;
    }

    return source;
}

[[nodiscard]] std::string CleanupScalarSwizzleArtifacts(std::string source)
{
    const std::regex scalarized_vec2_re(
        R"(max\(\((.+?\.[xyzwrgba])\)\.x,\s*\(\1\)\.y\))",
        std::regex::ECMAScript);
    source = std::regex_replace(source, scalarized_vec2_re, "$1");

    const std::regex scalarized_vec3_re(
        R"(max\(max\(\((.+?\.[xyzwrgba])\)\.x,\s*\(\1\)\.y\),\s*\(\1\)\.z\))",
        std::regex::ECMAScript);
    source = std::regex_replace(source, scalarized_vec3_re, "$1");

    return source;
}

[[nodiscard]] std::string RewriteAudioBarsUintModulo(std::string source)
{
    source = std::regex_replace(
        source,
        std::regex(
            R"(\buint\s+barFreq1\s*=\s*frequency\s*%\s*([0-9]+)\s*;)",
            std::regex::ECMAScript),
        "uint barFreq1 = uint(frequency) % $1u;");
    source = std::regex_replace(
        source,
        std::regex(
            R"(\buint\s+barFreq2\s*=\s*\(barFreq1\s*\+\s*1\s*\)\s*%\s*([0-9]+)\s*;)",
            std::regex::ECMAScript),
        "uint barFreq2 = (barFreq1 + 1u) % $1u;");
    source = std::regex_replace(
        source,
        std::regex(
            R"(\buint\s+barFreq1\s*=\s*frequency\s*%\s*RESOLUTION\s*;)",
            std::regex::ECMAScript),
        "uint barFreq1 = uint(frequency) % uint(RESOLUTION);");
    source = std::regex_replace(
        source,
        std::regex(
            R"(\(barFreq1\s*\+\s*1\s*\)\s*%\s*RESOLUTION)",
            std::regex::ECMAScript),
        "(barFreq1 + 1u) % uint(RESOLUTION)");
    return source;
}

[[nodiscard]] std::string RewriteFloatModuloAssignments(std::string source)
{
    const std::regex float_mod_re(
        R"(^(\s*(?:float\s+[A-Za-z_]\w+|[A-Za-z_]\w*\.[xyzwrgba])\s*=\s*)(.+)\s%\s(.+);\r?$)");

    wallpaper::usize search_pos = 0;
    while (search_pos < source.size()) {
        const wallpaper::usize line_end = source.find('\n', search_pos);
        const wallpaper::usize slice_end = line_end == std::string::npos ? source.size() : line_end;
        std::string line = source.substr(search_pos, slice_end - search_pos);
        std::smatch match;

        if (std::regex_match(line, match, float_mod_re)) {
            const std::string prefix = match[1].str();
            const std::string lhs = trimCopy(match[2].str());
            const std::string rhs = trimCopy(match[3].str());
            const std::string rewritten = prefix + "fmod(" + lhs + ", " + rhs + ");";
            source.replace(search_pos, slice_end - search_pos, rewritten);
            search_pos += rewritten.size();
            continue;
        }

        search_pos = line_end == std::string::npos ? source.size() : line_end + 1;
    }

    return source;
}

[[nodiscard]] std::string RewriteStepToFloatAssignments(std::string source)
{
    return std::regex_replace(
        source,
        std::regex(
            R"STEP(\bint(\s+[A-Za-z_][A-Za-z0-9_]*\s*=\s*step\s*\([^;\n]*\);))STEP",
            std::regex::ECMAScript),
        "float$1");
}

[[nodiscard]] std::string RewriteAlphaToCoverageDerivatives(std::string source)
{
    static const std::regex a2c_re(
        R"(gl_FragColor\.a\s*=\s*(saturate\s*\()?\s*\(\s*gl_FragColor\.a\s*-\s*0\.5\s*\)\s*/\s*max\s*\(\s*fwidth\s*\(\s*gl_FragColor\.a\s*\)\s*,\s*0\.0001\s*\)\s*\+\s*0\.5\s*\)?\s*;)",
        std::regex::ECMAScript);

    std::smatch match;
    if (!std::regex_search(source, match, a2c_re)) {
        return source;
    }

    const bool use_saturate = match[1].matched;
    const wallpaper::usize color_decl_pos = source.rfind("vec4 color =", match.position());
    if (color_decl_pos == std::string::npos) {
        return source;
    }
    const wallpaper::usize color_decl_end = source.find(';', color_decl_pos);
    if (color_decl_end == std::string::npos) {
        return source;
    }

    const std::string replacement =
        use_saturate
        ? "gl_FragColor.a = saturate(color.a);"
        : "gl_FragColor.a = color.a;";
    source.replace(match.position(), match.length(), replacement);
    return source;
}

[[nodiscard]] std::string RewriteBoolToFloatExpressions(std::string source)
{
    source = std::regex_replace(
        source,
        std::regex(
            R"BOOL(\(([^()]+(?:<=|>=|==|!=|<|>)[^()]+)\)\s*\*\s*)BOOL",
            std::regex::ECMAScript),
        "(($1) ? 1.0 : 0.0) * ");

    const std::regex bool_assign_re(
        R"(^(\s*(?:float\s+[A-Za-z_]\w+|[A-Za-z_]\w*\.[xyzwrgba])\s*=\s*)([^;]+(?:<=|>=|==|!=|<|>)[^;]+);\r?$)");

    wallpaper::usize search_pos = 0;
    while (search_pos < source.size()) {
        const wallpaper::usize line_end = source.find('\n', search_pos);
        const wallpaper::usize slice_end = line_end == std::string::npos ? source.size() : line_end;
        std::string line = source.substr(search_pos, slice_end - search_pos);
        std::smatch match;

        if (std::regex_match(line, match, bool_assign_re)) {
            const std::string prefix = match[1].str();
            const std::string expression = trimCopy(match[2].str());
            const std::string rewritten = prefix + "((" + expression + ") ? 1.0 : 0.0);";
            source.replace(search_pos, slice_end - search_pos, rewritten);
            search_pos += rewritten.size();
            continue;
        }

        search_pos = line_end == std::string::npos ? source.size() : line_end + 1;
    }

    return source;
}

[[nodiscard]] std::string RewriteIntegerForLoopBounds(std::string source)
{
    const std::regex loop_re(
        R"(^(\s*)for\s*\(\s*int\s+([A-Za-z_]\w*)\s*=\s*([^;]+);\s*([A-Za-z_]\w*)\s*([<>]=?)\s*([^;]+);\s*([^)]*)\)(.*)\r?$)");

    wallpaper::usize search_pos = 0;
    while (search_pos < source.size()) {
        const wallpaper::usize line_end = source.find('\n', search_pos);
        const wallpaper::usize slice_end = line_end == std::string::npos ? source.size() : line_end;
        std::string line = source.substr(search_pos, slice_end - search_pos);
        std::smatch match;

        if (std::regex_match(line, match, loop_re)) {
            const std::string indent = match[1].str();
            const std::string counter = match[2].str();
            const std::string begin = trimCopy(match[3].str());
            const std::string condition_counter = match[4].str();
            const std::string compare = match[5].str();
            const std::string end = trimCopy(match[6].str());
            const std::string advance = trimCopy(match[7].str());
            const std::string suffix = match[8].str();

            if (condition_counter != counter) {
                search_pos = line_end == std::string::npos ? source.size() : line_end + 1;
                continue;
            }

            const std::string rewritten =
                indent + "for (int " + counter + " = int(" + begin + "); " + counter + " " + compare +
                " int(" + end + "); " + advance + ")" + suffix;
            source.replace(search_pos, slice_end - search_pos, rewritten);
            search_pos += rewritten.size();
            continue;
        }

        search_pos = line_end == std::string::npos ? source.size() : line_end + 1;
    }

    return source;
}

[[nodiscard]] std::string RewriteMutableInputs(std::string source, wallpaper::ShaderType type)
{
    struct MutableInput
    {
        std::string type;
        std::string name;
        std::string renamed;
    };

    std::vector<MutableInput> mutable_inputs;
    const std::string qualifier_pattern =
        type == wallpaper::ShaderType::FRAGMENT ? "(?:in|varying)" : "(?:in|attribute)";
    const std::regex input_re(
        "(^|\\n)(\\s*)" + qualifier_pattern + R"(\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*;)",
        std::regex::ECMAScript);

    for (auto it = std::sregex_iterator(source.begin(), source.end(), input_re);
         it != std::sregex_iterator();
         ++it) {
        const std::string type = (*it)[3].str();
        const std::string name = (*it)[4].str();
        const std::regex assign_re(
            "\\b" + name + R"(\b(?:\s*\.[A-Za-z_][A-Za-z0-9_]*)?\s*(?:[+\-*/]?=))",
            std::regex::ECMAScript);
        if (std::regex_search(source.begin(), source.end(), assign_re)) {
            mutable_inputs.push_back(MutableInput {
                .type = type,
                .name = name,
                .renamed = name + "_Input",
            });
        }
    }

    if (mutable_inputs.empty()) {
        return source;
    }

    for (const auto& input : mutable_inputs) {
        const std::regex declaration_re(
            "(^|\\n)(\\s*)" + qualifier_pattern + R"(\s+)" + input.type + R"(\s+)" + input.name +
                R"(\s*;)",
            std::regex::ECMAScript);
        source = std::regex_replace(
            source,
            declaration_re,
            "$1$2in " + input.type + " " + input.renamed + ";");
    }

    const wallpaper::usize main_pos = source.find("void main()");
    if (main_pos == std::string::npos) {
        return source;
    }
    const wallpaper::usize brace_pos = source.find('{', main_pos);
    if (brace_pos == std::string::npos) {
        return source;
    }

    std::string local_copies;
    for (const auto& input : mutable_inputs) {
        local_copies += "\n " + input.type + " " + input.name + " = " + input.renamed + ";\n";
    }
    source.insert(brace_pos + 1, local_copies);
    return source;
}

void ExtractStageInterface(
    std::string_view source,
    wallpaper::WPPreprocessorInfo& info,
    wallpaper::ShaderType type)
{
    const std::string source_string(source);
    const std::regex io_re(
        R"((^|\n)\s*((?:layout\s*\([^)]*\)\s*)?)(in|out|varying|attribute)\s+[\s\w]+\s(\w+)\s*;)",
        std::regex::ECMAScript);
    for (auto it = std::sregex_iterator(source_string.begin(), source_string.end(), io_re);
         it != std::sregex_iterator();
         ++it) {
        const std::smatch match = *it;
        const auto declaration = trimCopy(match[0].str());
        const std::string qualifier = match[3].str();
        const bool is_input =
            qualifier == "in" || qualifier == "attribute" ||
            (qualifier == "varying" && type == wallpaper::ShaderType::FRAGMENT);
        if (is_input) {
            const std::string name = match[4].str();
            if (name != "gl_Position" && name != "_ww_sv_position") {
                info.input[name] = declaration;
            }
        } else {
            const std::string name = match[4].str();
            if (name != "gl_Position" && name != "_ww_sv_position") {
                info.output[name] = declaration;
            }
        }
    }
}

void ExtractActiveTextureSlots(std::string_view source, wallpaper::WPPreprocessorInfo& info)
{
    const std::string source_string(source);
    const std::regex tex_re(R"(uniform\s+sampler2D\s+g_Texture(\d+))", std::regex::ECMAScript);
    for (auto it = std::sregex_iterator(source_string.begin(), source_string.end(), tex_re);
         it != std::sregex_iterator();
         ++it) {
        const std::smatch match = *it;
        const auto slot_string = match[1].str();
        unsigned int slot = 0;
        char* end = nullptr;
        slot = static_cast<unsigned int>(std::strtoul(slot_string.c_str(), &end, 10));
        if (end != nullptr && *end == '\0') {
            info.active_tex_slots.insert(slot);
        }
    }
}

} // namespace

namespace wallpaper::shader
{

StructuredStageSource LegalizeStageSource(
    std::string_view preprocessed_source,
    ShaderType type)
{
    StructuredStageSource result {};
    std::map<std::string, int64_t> macros;

    result.source = StripInactiveConditionals(preprocessed_source, macros);
    result.source = ApplyLegalizerPass(
        "RewriteMutableInputs",
        std::move(result.source),
        [type](std::string source) { return RewriteMutableInputs(std::move(source), type); });
    result.source = ApplyLegalizerPass(
        "RewriteStageIdentifierCollisions",
        std::move(result.source),
        [](std::string source) { return RewriteStageIdentifierCollisions(std::move(source)); });
    result.source = ApplyLegalizerPass(
        "RewriteTextureInitializerCastVec2",
        std::move(result.source),
        [](std::string source) { return RewriteTextureInitializerCast(std::move(source), "vec2"); });
    result.source = ApplyLegalizerPass(
        "RewriteTextureInitializerCastVec3",
        std::move(result.source),
        [](std::string source) { return RewriteTextureInitializerCast(std::move(source), "vec3"); });
    result.source = ApplyLegalizerPass(
        "RewriteScalarTextureSampleInitializers",
        std::move(result.source),
        [](std::string source) { return RewriteScalarTextureSampleInitializers(std::move(source)); });
    result.source = ApplyLegalizerPass(
        "RewriteMultiVectorScalarDeclarations",
        std::move(result.source),
        [](std::string source) { return RewriteMultiVectorScalarDeclarations(std::move(source)); });
    result.source = ApplyLegalizerPass(
        "RewriteUserDefinedModFunction",
        std::move(result.source),
        [](std::string source) { return RewriteUserDefinedModFunction(std::move(source)); });
    result.source = ApplyLegalizerPass(
        "RewriteNumericMacroTernaryConditions",
        std::move(result.source),
        [&macros](std::string source) {
            return RewriteNumericMacroTernaryConditions(std::move(source), macros);
        });
    result.source = ApplyLegalizerPass(
        "RewriteVec3Vec2BinaryOps",
        std::move(result.source),
        [](std::string source) { return RewriteVec3Vec2BinaryOps(std::move(source)); });
    result.source = ApplyLegalizerPass(
        "RewriteVectorWidthInitializers",
        std::move(result.source),
        [](std::string source) { return RewriteVectorWidthInitializers(std::move(source)); });
    result.source = ApplyLegalizerPass(
        "RewriteVectorWidthBuiltinAssignments",
        std::move(result.source),
        [](std::string source) { return RewriteVectorWidthBuiltinAssignments(std::move(source)); });
    result.source = ApplyLegalizerPass(
        "RewriteBuiltinIntegerLiteralArguments",
        std::move(result.source),
        [](std::string source) { return RewriteBuiltinIntegerLiteralArguments(std::move(source)); });
    result.source = ApplyLegalizerPass(
        "RewriteBuiltinVectorScalarArguments",
        std::move(result.source),
        [](std::string source) { return RewriteBuiltinVectorScalarArguments(std::move(source)); });
    result.source = ApplyLegalizerPass(
        "RewriteVectorScalarMaxCalls",
        std::move(result.source),
        [](std::string source) { return RewriteVectorScalarMaxCalls(std::move(source)); });
    result.source = ApplyLegalizerPass(
        "RewriteFloatDeclarationsToVectors",
        std::move(result.source),
        [](std::string source) { return RewriteFloatDeclarationsToVectors(std::move(source)); });
    result.source = ApplyLegalizerPass(
        "RewriteFloatVectorInitializers",
        std::move(result.source),
        [](std::string source) { return RewriteFloatVectorInitializers(std::move(source)); });
    result.source = ApplyLegalizerPass(
        "CleanupScalarSwizzleArtifacts",
        std::move(result.source),
        [](std::string source) { return CleanupScalarSwizzleArtifacts(std::move(source)); });
    result.source = ApplyLegalizerPass(
        "RewriteAudioBarsUintModulo",
        std::move(result.source),
        [](std::string source) { return RewriteAudioBarsUintModulo(std::move(source)); });
    result.source = ApplyLegalizerPass(
        "RewriteFloatModuloAssignments",
        std::move(result.source),
        [](std::string source) { return RewriteFloatModuloAssignments(std::move(source)); });
    result.source = ApplyLegalizerPass(
        "RewriteStepToFloatAssignments",
        std::move(result.source),
        [](std::string source) { return RewriteStepToFloatAssignments(std::move(source)); });
    result.source = ApplyLegalizerPass(
        "RewriteAlphaToCoverageDerivatives",
        std::move(result.source),
        [](std::string source) { return RewriteAlphaToCoverageDerivatives(std::move(source)); });
    result.source = ApplyLegalizerPass(
        "RewriteBoolToFloatExpressions",
        std::move(result.source),
        [](std::string source) { return RewriteBoolToFloatExpressions(std::move(source)); });
    result.source = ApplyLegalizerPass(
        "RewriteBoolArithmetic",
        std::move(result.source),
        [](std::string source) { return RewriteBoolArithmetic(std::move(source)); });
    result.source = ApplyLegalizerPass(
        "RewriteIntegerForLoopBounds",
        std::move(result.source),
        [](std::string source) { return RewriteIntegerForLoopBounds(std::move(source)); });

    ExtractStageInterface(result.source, result.preprocess_info, type);
    ExtractActiveTextureSlots(result.source, result.preprocess_info);
    return result;
}

} // namespace wallpaper::shader
