// youtube-tv - Signature and "n" descrambler for ciphered stream URLs.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
//
// A client that gates playback behind the player's own signature answers with
// signatureCipher instead of a plain URL. Undoing it is what YouTube's player
// JS does with two small helper functions, so this fetches that file and
// extracts them by structure, not by literal anchors.
//
// A plain build names its helpers directly from the "signature"/"n" setters.
// Today's TV build obfuscates them: a ";".split(";") string table, an
// operation table of splice/reverse/swap, and one dispatcher that drives them
// through computed member calls. The TV signature helper's selector arguments
// are read from its own call sites and the one that transforms a sample is
// kept. A helper is only taken as the throttle when it is reached from the
// player's own "n" call site; the TV build's n path is a VM this subset cannot
// run, so no throttle helper is claimed for it and the URL is left alone.
//
// The helpers run on a bounded JS subset - split/join/reverse/slice/splice/
// swaps, for/while with an index, charCodeAt/fromCharCode, array literals and
// pushes, the arithmetic of the transforms, bitwise and short-circuit
// operators, and encode/decodeURIComponent - instead of embedding a JS engine.
// Every loop stays inside the same global step cap, and anything outside the
// subset fails with the operation named, so the log says what was not
// understood instead of hanging on a hostile page. descramble always applies
// the signature to a query "s"; only when a throttle helper was proven does it
// rewrite a query "n" and the TV path shape "/n/<value>/videoplayback".

#include "yt/decipher.hpp"

#include "yt/http.hpp"
#include "yt/log.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace yt
{
namespace
{

// A hostile or corrupt page must not be able to hang the worker: the file is
// bounded at the transport, the extracted helpers are bounded again, and the
// interpreter stops after a fixed number of steps.
constexpr std::size_t kMaxJsBytes = 3u << 20;
constexpr std::size_t kMaxFunctionBytes = 256u << 10;
constexpr int kMaxSteps = 200000;
constexpr const char *kPlayerUserAgent = "Mozilla/5.0 (ChromiumStylePlatform) Cobalt/Version";

bool ident_start(char ch)
{
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_' || ch == '$';
}

bool ident_char(char ch)
{
    return ident_start(ch) || (ch >= '0' && ch <= '9');
}

bool is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r' || ch == '\f' || ch == '\v';
}

void skip_space(const std::string &text, std::size_t *at)
{
    while (*at < text.size() && is_space(text[*at]))
        ++*at;
}

std::string_view trim(std::string_view text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && is_space(text[begin]))
        ++begin;
    while (end > begin && is_space(text[end - 1]))
        --end;
    return text.substr(begin, end - begin);
}

bool is_identifier(std::string_view text)
{
    if (text.empty() || !ident_start(text[0]))
        return false;
    for (char ch : text)
    {
        if (!ident_char(ch))
            return false;
    }
    return true;
}

// One past the closing quote of the string that opens at text[at].
std::size_t skip_string(const std::string &text, std::size_t at)
{
    const char quote = text[at];
    for (std::size_t index = at + 1; index < text.size(); ++index)
    {
        if (text[index] == '\\')
        {
            ++index;
            continue;
        }
        if (text[index] == quote)
            return index + 1;
    }
    return text.size();
}

// One past the bracket that matches text[open], skipping strings and comments;
// npos when it never closes.
std::size_t skip_brackets(const std::string &text, std::size_t open)
{
    const char oc = text[open];
    const char cc = oc == '{' ? '}' : (oc == '(' ? ')' : ']');
    int depth = 0;
    for (std::size_t index = open; index < text.size(); ++index)
    {
        const char ch = text[index];
        if (ch == '"' || ch == '\'' || ch == '`')
        {
            index = skip_string(text, index) - 1;
            continue;
        }
        if (ch == '/' && index + 1 < text.size() && text[index + 1] == '/')
        {
            const std::size_t end = text.find('\n', index);
            if (end == std::string::npos)
                return std::string::npos;
            index = end;
            continue;
        }
        if (ch == '/' && index + 1 < text.size() && text[index + 1] == '*')
        {
            const std::size_t end = text.find("*/", index);
            if (end == std::string::npos)
                return std::string::npos;
            index = end + 1;
            continue;
        }
        if (ch == oc)
            ++depth;
        else if (ch == cc)
        {
            --depth;
            if (depth == 0)
                return index + 1;
        }
    }
    return std::string::npos;
}

struct JsFunction
{
    std::vector<std::string> params;
    std::string body;
};

// Parse the function whose "function" keyword is at `at`. Fills name (empty
// when anonymous) and out; sets *end one past the closing brace.
bool parse_function(const std::string &text, std::size_t at, std::string *name, JsFunction *out,
                    std::size_t *end)
{
    std::size_t index = at + std::strlen("function");
    skip_space(text, &index);
    name->clear();
    if (index < text.size() && ident_start(text[index]))
    {
        const std::size_t begin = index;
        while (index < text.size() && ident_char(text[index]))
            ++index;
        *name = text.substr(begin, index - begin);
        skip_space(text, &index);
    }
    if (index >= text.size() || text[index] != '(')
        return false;
    const std::size_t params_end = skip_brackets(text, index);
    if (params_end == std::string::npos)
        return false;
    const std::string_view params =
        std::string_view(text).substr(index + 1, params_end - index - 2);
    out->params.clear();
    std::size_t part = 0;
    while (part <= params.size())
    {
        const std::size_t comma = params.find(',', part);
        const std::size_t length =
            comma == std::string_view::npos ? params.size() - part : comma - part;
        const std::string_view one = trim(params.substr(part, length));
        if (!one.empty())
            out->params.emplace_back(one);
        if (comma == std::string_view::npos)
            break;
        part = comma + 1;
    }
    index = params_end;
    skip_space(text, &index);
    if (index >= text.size() || text[index] != '{')
        return false;
    const std::size_t body_end = skip_brackets(text, index);
    if (body_end == std::string::npos)
        return false;
    out->body = text.substr(index + 1, body_end - index - 2);
    *end = body_end;
    return true;
}

// "function name(...)" or "name=function(...)".
bool find_named_function(const std::string &js, const std::string &name, JsFunction *out)
{
    std::size_t at = 0;
    while ((at = js.find(name, at)) != std::string::npos)
    {
        const std::size_t after = at + name.size();
        const bool left_ok = at == 0 || !ident_char(js[at - 1]);
        const bool right_ok = after >= js.size() || !ident_char(js[after]);
        if (left_ok && right_ok)
        {
            std::size_t probe = after;
            skip_space(js, &probe);
            if (probe < js.size() && js[probe] == '=')
            {
                std::size_t fn = probe + 1;
                skip_space(js, &fn);
                if (js.compare(fn, std::strlen("function"), "function") == 0)
                {
                    std::string parsed;
                    std::size_t end = 0;
                    if (parse_function(js, fn, &parsed, out, &end))
                        return true;
                }
            }
        }
        at = after;
    }

    at = 0;
    while ((at = js.find("function", at)) != std::string::npos)
    {
        std::string parsed;
        std::size_t end = 0;
        JsFunction candidate;
        if (parse_function(js, at, &parsed, &candidate, &end) && parsed == name)
        {
            *out = std::move(candidate);
            return true;
        }
        at += std::strlen("function");
    }
    return false;
}

// The pattern at brace depth zero of an object body ("Xq:" rather than a
// nested "Xq:" inside another method).
std::size_t find_at_depth(const std::string &text, const std::string &pattern)
{
    int depth = 0;
    for (std::size_t index = 0; index + pattern.size() <= text.size(); ++index)
    {
        const char ch = text[index];
        if (ch == '"' || ch == '\'' || ch == '`')
        {
            index = skip_string(text, index) - 1;
            continue;
        }
        if (ch == '{')
            ++depth;
        else if (ch == '}')
            --depth;
        else if (depth == 0 && text.compare(index, pattern.size(), pattern) == 0)
            return index;
    }
    return std::string::npos;
}

// "name.key:function(...){...}" inside the object literal assigned to `object`.
bool find_table_method(const std::string &js, const std::string &object, const std::string &key,
                       JsFunction *out)
{
    std::size_t at = 0;
    while ((at = js.find(object, at)) != std::string::npos)
    {
        const std::size_t after = at + object.size();
        if (at != 0 && ident_char(js[at - 1]))
        {
            at = after;
            continue;
        }
        std::size_t probe = after;
        skip_space(js, &probe);
        std::size_t brace = probe;
        if (probe < js.size() && js[probe] == '=')
        {
            brace = probe + 1;
            skip_space(js, &brace);
        }
        if (brace < js.size() && js[brace] == '{')
        {
            const std::size_t table_end = skip_brackets(js, brace);
            if (table_end == std::string::npos)
                return false;
            const std::string body = js.substr(brace + 1, table_end - brace - 2);
            for (const std::string &pattern : {key + ":", "\"" + key + "\":", "'" + key + "':"})
            {
                const std::size_t entry = find_at_depth(body, pattern);
                if (entry == std::string::npos)
                    continue;
                std::size_t fn = body.find("function", entry + pattern.size());
                if (fn == std::string::npos)
                    continue;
                std::string parsed;
                std::size_t end = 0;
                if (parse_function(body, fn, &parsed, out, &end))
                    return true;
            }
        }
        at = after;
    }
    return false;
}

// Resolves the helper names the two setters mention, caching what it finds.
class Script
{
  public:
    explicit Script(const std::string &js) : js_(js)
    {
    }

    const JsFunction *find(const std::string &name)
    {
        const auto cached = cache_.find(name);
        if (cached != cache_.end())
            return &cached->second;
        JsFunction fn;
        const std::size_t dot = name.find('.');
        bool found = false;
        if (dot == std::string::npos)
            found = find_named_function(js_, name, &fn);
        else
            found = find_table_method(js_, name.substr(0, dot), name.substr(dot + 1), &fn);
        if (!found)
            return nullptr;
        return &cache_.emplace(name, std::move(fn)).first->second;
    }

  private:
    const std::string &js_;
    std::map<std::string, JsFunction> cache_;
};

struct Value
{
    enum class Kind : unsigned char
    {
        none,
        number,
        string,
        array, // a mutable array of characters (splice/reverse/swap operate on it)
        ctor,  // the bare identifier "String", before .fromCharCode
        words, // an array of strings, e.g. the player's own ";".split(";") table
        table, // an object of helper functions, e.g. {dl:function(a,b){...}}
        fn,    // a function value held in a table
    };

    Kind kind = Kind::none;
    double number = 0.0;
    std::string text;
    std::shared_ptr<std::string> array;
    std::shared_ptr<std::vector<std::string>> words;
    std::shared_ptr<std::map<std::string, JsFunction>> table;
    std::shared_ptr<JsFunction> fn;
};

bool truthy(const Value &value)
{
    switch (value.kind)
    {
    case Value::Kind::number:
        return value.number != 0.0;
    case Value::Kind::string:
        return !value.text.empty();
    case Value::Kind::array:
        return !value.array->empty();
    case Value::Kind::words:
        return !value.words->empty();
    default:
        return value.kind != Value::Kind::none;
    }
}

Value number_value(double value)
{
    Value out;
    out.kind = Value::Kind::number;
    out.number = value;
    return out;
}

Value string_value(std::string text)
{
    Value out;
    out.kind = Value::Kind::string;
    out.text = std::move(text);
    return out;
}

Value array_value(std::string chars)
{
    Value out;
    out.kind = Value::Kind::array;
    out.array = std::make_shared<std::string>(std::move(chars));
    return out;
}

// Defined with the URL helpers below; the interpreter's builtins need them.
std::string percent_decode(std::string_view text);
std::string encode_uri_component(std::string_view text);

// Interprets the bounded JS subset over one helper body. Each call runs in its
// own frame; arrays are shared by pointer, so a splice or swap inside a helper
// mutates the array the caller holds.
class Interp
{
  public:
    explicit Interp(Script *script) : script_(script)
    {
    }

    // Names the whole player defines once (its string table, its operation
    // tables); every interpreted frame starts with them bound.
    void set_globals(const std::map<std::string, Value> *globals)
    {
        globals_ = globals;
    }

    bool run(const JsFunction &function, const std::vector<Value> &args, Value *out,
             std::string *error)
    {
        Frame frame{function.body, 0, {}};
        if (globals_ != nullptr)
            frame.vars = *globals_;
        for (std::size_t index = 0; index < function.params.size(); ++index)
            frame.vars[function.params[index]] = index < args.size() ? args[index] : Value{};
        bool returned = false;
        if (!statements(frame, out, &returned, error))
            return false;
        if (!returned)
            *out = Value{};
        return true;
    }

  private:
    struct Frame
    {
        const std::string &body;
        std::size_t at;
        std::map<std::string, Value> vars;
    };

    Script *script_;
    const std::map<std::string, Value> *globals_ = nullptr;
    int steps_ = 0;

    bool tick(std::string *error)
    {
        if (++steps_ > kMaxSteps)
        {
            *error = "player JS expression budget exceeded";
            return false;
        }
        return true;
    }

    bool statements(Frame &frame, Value *result, bool *returned, std::string *error)
    {
        while (true)
        {
            skip_space(frame.body, &frame.at);
            while (frame.at < frame.body.size() && frame.body[frame.at] == ';')
            {
                ++frame.at;
                skip_space(frame.body, &frame.at);
            }
            if (frame.at >= frame.body.size() || frame.body[frame.at] == '}')
                return true;
            if (!tick(error))
                return false;
            if (!statement(frame, result, returned, error))
                return false;
            if (*returned)
                return true;
        }
    }

    bool statement(Frame &frame, Value *result, bool *returned, std::string *error)
    {
        const std::size_t start = frame.at;
        if (keyword(frame.body, start, "return"))
        {
            frame.at = start + std::strlen("return");
            Value value;
            if (!expression(frame, &value, error))
                return false;
            *result = std::move(value);
            *returned = true;
            return true;
        }
        if (keyword(frame.body, start, "var"))
        {
            frame.at = start + std::strlen("var");
            return declaration(frame, error);
        }
        if (keyword(frame.body, start, "if"))
        {
            frame.at = start + std::strlen("if");
            return if_statement(frame, result, returned, error);
        }
        if (keyword(frame.body, start, "while"))
        {
            frame.at = start + std::strlen("while");
            return while_statement(frame, result, returned, error);
        }
        if (keyword(frame.body, start, "for"))
        {
            frame.at = start + std::strlen("for");
            return for_statement(frame, result, returned, error);
        }
        if (frame.body[frame.at] == '{')
            return block(frame, result, returned, error);
        if (frame.body[frame.at] == '[')
            return destructuring(frame, error);
        if (update_expression(frame, error))
            return true;

        const std::size_t assign = find_assign(frame.body, start);
        if (assign != std::string::npos)
        {
            const std::string target(frame.body.substr(start, assign - start));
            frame.at = assign + 1;
            Value value;
            if (!expression(frame, &value, error))
                return false;
            if (compound_target(target))
                return compound_assign(frame, target, value, error);
            return assign_target(frame, target, value, error);
        }

        Value discard;
        return expression(frame, &discard, error);
    }

    // "x += y" reaches find_assign as the target "x +".
    static bool compound_target(const std::string &target)
    {
        const std::string_view text = trim(target);
        if (text.empty())
            return false;
        const char op = text.back();
        return op == '+' || op == '-' || op == '*' || op == '/' || op == '%' || op == '&' ||
               op == '|' || op == '^';
    }

    bool compound_assign(Frame &frame, const std::string &target, const Value &right,
                         std::string *error)
    {
        std::string_view text = trim(target);
        if (text.size() < 2)
        {
            *error = "unsupported assignment target: " + std::string(text);
            return false;
        }
        const char op = text.back();
        text = trim(text.substr(0, text.size() - 1));
        if (!is_identifier(text))
        {
            *error = "unsupported assignment target: " + std::string(text);
            return false;
        }
        const auto known = frame.vars.find(std::string(text));
        if (known == frame.vars.end())
        {
            *error = "assignment to an unknown name: " + std::string(text);
            return false;
        }
        Value current = known->second;
        if (op == '+' && current.kind == Value::Kind::string && right.kind == Value::Kind::string)
            current.text += right.text;
        else if (op == '+' && current.kind == Value::Kind::string &&
                 right.kind == Value::Kind::number)
        {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), "%g", right.number);
            current.text += buffer;
        }
        else if (current.kind == Value::Kind::number && right.kind == Value::Kind::number)
        {
            switch (op)
            {
            case '+':
                current.number += right.number;
                break;
            case '-':
                current.number -= right.number;
                break;
            case '*':
                current.number *= right.number;
                break;
            case '/':
                current.number = right.number == 0.0 ? 0.0 : current.number / right.number;
                break;
            case '%':
            {
                const long long divisor = static_cast<long long>(right.number);
                current.number =
                    divisor == 0
                        ? 0.0
                        : static_cast<double>(static_cast<long long>(current.number) % divisor);
                break;
            }
            case '&':
                current.number = static_cast<double>(static_cast<long long>(current.number) &
                                                     static_cast<long long>(right.number));
                break;
            case '|':
                current.number = static_cast<double>(static_cast<long long>(current.number) |
                                                     static_cast<long long>(right.number));
                break;
            case '^':
                current.number = static_cast<double>(static_cast<long long>(current.number) ^
                                                     static_cast<long long>(right.number));
                break;
            default:
                break;
            }
        }
        else
        {
            *error = std::string("unsupported compound assignment '") + op + "='";
            return false;
        }
        known->second = std::move(current);
        return true;
    }

    bool block(Frame &frame, Value *result, bool *returned, std::string *error)
    {
        ++frame.at; // '{'
        if (!statements(frame, result, returned, error))
            return false;
        skip_space(frame.body, &frame.at);
        if (frame.at >= frame.body.size() || frame.body[frame.at] != '}')
        {
            *error = "unbalanced { in block";
            return false;
        }
        ++frame.at;
        return true;
    }

    // A body that is either a block or one statement, executed or skipped.
    bool branch(Frame &frame, bool run, Value *result, bool *returned, std::string *error)
    {
        skip_space(frame.body, &frame.at);
        if (frame.at < frame.body.size() && frame.body[frame.at] == '{')
        {
            if (!run)
            {
                const std::size_t end = skip_brackets(frame.body, frame.at);
                if (end == std::string::npos)
                {
                    *error = "unbalanced { in block";
                    return false;
                }
                frame.at = end;
                return true;
            }
            return block(frame, result, returned, error);
        }
        if (!run)
        {
            skip_to_statement_end(frame);
            return true;
        }
        return statement(frame, result, returned, error);
    }

    bool if_statement(Frame &frame, Value *result, bool *returned, std::string *error)
    {
        skip_space(frame.body, &frame.at);
        if (frame.at >= frame.body.size() || frame.body[frame.at] != '(')
        {
            *error = "unsupported if";
            return false;
        }
        ++frame.at;
        Value condition;
        if (!expression(frame, &condition, error))
            return false;
        skip_space(frame.body, &frame.at);
        if (frame.at >= frame.body.size() || frame.body[frame.at] != ')')
        {
            *error = "unbalanced ( in if";
            return false;
        }
        ++frame.at;
        const bool taken = truthy(condition);
        if (!branch(frame, taken, result, returned, error))
            return false;
        if (*returned)
            return true;
        skip_space(frame.body, &frame.at);
        const std::size_t else_at = frame.at;
        if (keyword(frame.body, else_at, "else"))
        {
            frame.at = else_at + std::strlen("else");
            if (!branch(frame, !taken, result, returned, error))
                return false;
        }
        return true;
    }

    bool while_statement(Frame &frame, Value *result, bool *returned, std::string *error)
    {
        skip_space(frame.body, &frame.at);
        if (frame.at >= frame.body.size() || frame.body[frame.at] != '(')
        {
            *error = "unsupported while";
            return false;
        }
        const std::size_t condition_begin = frame.at + 1;
        const std::size_t close = skip_brackets(frame.body, frame.at);
        if (close == std::string::npos)
        {
            *error = "unbalanced ( in while";
            return false;
        }
        const std::size_t body_at = close;
        while (true)
        {
            frame.at = condition_begin;
            Value next;
            if (!expression(frame, &next, error))
                return false;
            if (!truthy(next))
                break;
            if (!tick(error))
                return false;
            frame.at = body_at;
            if (!branch(frame, true, result, returned, error))
                return false;
            if (*returned)
                return true;
        }
        frame.at = body_at;
        return branch(frame, false, result, returned, error);
    }

    bool for_statement(Frame &frame, Value *result, bool *returned, std::string *error)
    {
        skip_space(frame.body, &frame.at);
        if (frame.at >= frame.body.size() || frame.body[frame.at] != '(')
        {
            *error = "unsupported for";
            return false;
        }
        const std::size_t open = frame.at;
        const std::size_t close = skip_brackets(frame.body, open);
        if (close == std::string::npos)
        {
            *error = "unbalanced ( in for";
            return false;
        }
        ++frame.at; // '('
        // init;
        skip_space(frame.body, &frame.at);
        if (frame.at < close && frame.body[frame.at] != ';')
        {
            if (keyword(frame.body, frame.at, "var"))
                frame.at += std::strlen("var");
            if (!declaration_or_expression(frame, error))
                return false;
        }
        skip_space(frame.body, &frame.at);
        if (frame.at >= close || frame.body[frame.at] != ';')
        {
            *error = "unsupported for";
            return false;
        }
        ++frame.at;
        // condition; (located, not run here)
        const std::size_t condition_begin = frame.at;
        const std::size_t condition_end = find_semi(frame.body, frame.at);
        if (condition_end == std::string::npos || condition_end > close)
        {
            *error = "unsupported for";
            return false;
        }
        const std::size_t step_begin = condition_end + 1;
        const std::size_t body_at = close;

        while (true)
        {
            frame.at = condition_begin;
            Value condition = number_value(1.0);
            if (frame.at < condition_end)
            {
                if (!expression(frame, &condition, error))
                    return false;
            }
            if (!truthy(condition))
                break;
            if (!tick(error))
                return false;
            frame.at = body_at;
            if (!branch(frame, true, result, returned, error))
                return false;
            if (*returned)
                return true;
            frame.at = step_begin;
            if (frame.at < close && !declaration_or_expression(frame, error))
                return false;
        }
        frame.at = body_at;
        return branch(frame, false, result, returned, error);
    }

    static std::size_t find_semi(const std::string &body, std::size_t from)
    {
        int depth = 0;
        for (std::size_t index = from; index < body.size(); ++index)
        {
            const char ch = body[index];
            if (ch == '"' || ch == '\'' || ch == '`')
            {
                index = skip_string(body, index) - 1;
                continue;
            }
            if (ch == '(' || ch == '[' || ch == '{')
                ++depth;
            else if (ch == ')' || ch == ']' || ch == '}')
            {
                if (depth == 0)
                    return std::string::npos;
                --depth;
            }
            else if (ch == ';' && depth == 0)
                return index;
        }
        return std::string::npos;
    }

    bool declaration_or_expression(Frame &frame, std::string *error)
    {
        const std::size_t start = frame.at;
        if (keyword(frame.body, start, "var"))
        {
            frame.at = start + std::strlen("var");
            return declaration(frame, error);
        }
        if (update_expression(frame, error))
            return true;
        const std::size_t assign = find_assign(frame.body, start);
        const std::size_t semi = frame.body.find(';', start);
        if (assign != std::string::npos && (semi == std::string::npos || assign < semi))
        {
            const std::string target(frame.body.substr(start, assign - start));
            frame.at = assign + 1;
            Value value;
            if (!expression(frame, &value, error))
                return false;
            if (compound_target(target))
                return compound_assign(frame, target, value, error);
            return assign_target(frame, target, value, error);
        }
        Value discard;
        return expression(frame, &discard, error);
    }

    // "i++", "i--", "++i" or "--i".
    bool update_expression(Frame &frame, std::string *error)
    {
        const std::size_t save = frame.at;
        skip_space(frame.body, &frame.at);
        const bool prefix = frame.body.compare(frame.at, 2, "++") == 0 ||
                            frame.body.compare(frame.at, 2, "--") == 0;
        int delta = 0;
        if (prefix)
        {
            delta = frame.body[frame.at] == '+' ? 1 : -1;
            frame.at += 2;
            skip_space(frame.body, &frame.at);
        }
        if (frame.at >= frame.body.size() || !ident_start(frame.body[frame.at]))
        {
            frame.at = save;
            return false;
        }
        const std::size_t begin = frame.at;
        while (frame.at < frame.body.size() && ident_char(frame.body[frame.at]))
            ++frame.at;
        const std::string name = frame.body.substr(begin, frame.at - begin);
        if (!prefix)
        {
            std::size_t probe = frame.at;
            skip_space(frame.body, &probe);
            if (frame.body.compare(probe, 2, "++") != 0 && frame.body.compare(probe, 2, "--") != 0)
            {
                frame.at = save;
                return false;
            }
            delta = frame.body[probe] == '+' ? 1 : -1;
            frame.at = probe + 2;
        }
        const auto known = frame.vars.find(name);
        if (known == frame.vars.end() || known->second.kind != Value::Kind::number)
        {
            *error = "unsupported update of a non-number: " + name;
            return false;
        }
        known->second.number += delta;
        return true;
    }

    // Advance past the rest of a statement without running it: used for the
    // right side of a short-circuited "||"/"&&", which the player's obfuscated
    // helpers fill with dead code.
    static void skip_to_statement_end(Frame &frame)
    {
        int depth = 0;
        while (frame.at < frame.body.size())
        {
            const char ch = frame.body[frame.at];
            if (ch == '"' || ch == '\'' || ch == '`')
            {
                frame.at = skip_string(frame.body, frame.at);
                continue;
            }
            if (ch == '(' || ch == '[' || ch == '{')
                ++depth;
            else if (ch == ')' || ch == ']' || ch == '}')
            {
                if (depth == 0)
                    return;
                --depth;
            }
            else if ((ch == ';' || ch == ',') && depth == 0)
                return;
            ++frame.at;
        }
    }

    bool declaration(Frame &frame, std::string *error)
    {
        while (true)
        {
            skip_space(frame.body, &frame.at);
            if (frame.at >= frame.body.size() || !ident_start(frame.body[frame.at]))
            {
                *error = "unsupported var declaration";
                return false;
            }
            const std::size_t begin = frame.at;
            while (frame.at < frame.body.size() && ident_char(frame.body[frame.at]))
                ++frame.at;
            const std::string name = frame.body.substr(begin, frame.at - begin);
            skip_space(frame.body, &frame.at);
            Value value;
            if (frame.at < frame.body.size() && frame.body[frame.at] == '=')
            {
                ++frame.at;
                if (!expression(frame, &value, error))
                    return false;
            }
            frame.vars[name] = std::move(value);
            skip_space(frame.body, &frame.at);
            if (frame.at < frame.body.size() && frame.body[frame.at] == ',')
            {
                ++frame.at;
                continue;
            }
            return true;
        }
    }

    bool destructuring(Frame &frame, std::string *error)
    {
        const std::size_t lhs_end = skip_brackets(frame.body, frame.at);
        if (lhs_end == std::string::npos)
        {
            *error = "unbalanced [ in swap";
            return false;
        }
        const std::string_view lhs =
            std::string_view(frame.body).substr(frame.at + 1, lhs_end - frame.at - 2);
        std::size_t at = lhs_end;
        skip_space(frame.body, &at);
        if (at >= frame.body.size() || frame.body[at] != '=')
        {
            *error = "unsupported destructuring";
            return false;
        }
        ++at;
        skip_space(frame.body, &at);
        if (at >= frame.body.size() || frame.body[at] != '[')
        {
            *error = "unsupported destructuring";
            return false;
        }
        const std::size_t rhs_end = skip_brackets(frame.body, at);
        if (rhs_end == std::string::npos)
        {
            *error = "unbalanced [ in swap";
            return false;
        }
        const std::string_view rhs = std::string_view(frame.body).substr(at + 1, rhs_end - at - 2);
        frame.at = rhs_end;

        const std::vector<std::string_view> targets = split_commas(lhs);
        const std::vector<std::string_view> sources = split_commas(rhs);
        if (targets.empty() || targets.size() != sources.size())
        {
            *error = "unsupported destructuring";
            return false;
        }
        std::vector<Value> values;
        values.reserve(sources.size());
        for (std::string_view source : sources)
        {
            Value value;
            if (!eval_value(frame, source, &value, error))
                return false;
            values.push_back(std::move(value));
        }
        for (std::size_t index = 0; index < targets.size(); ++index)
        {
            if (!assign_target(frame, std::string(targets[index]), values[index], error))
                return false;
        }
        return true;
    }

    bool expression(Frame &frame, Value *out, std::string *error)
    {
        return logical_or(frame, out, error);
    }

    // "||" and "&&" short-circuit: the player's obfuscated helpers put dead
    // code on the right, so it is skipped rather than run.
    bool logical_or(Frame &frame, Value *out, std::string *error)
    {
        Value left;
        if (!logical_and(frame, &left, error))
            return false;
        while (true)
        {
            skip_space(frame.body, &frame.at);
            if (frame.at + 1 >= frame.body.size() || frame.body[frame.at] != '|' ||
                frame.body[frame.at + 1] != '|')
                break;
            frame.at += 2;
            if (truthy(left))
            {
                skip_to_statement_end(frame);
                break;
            }
            if (!logical_and(frame, &left, error))
                return false;
        }
        *out = std::move(left);
        return true;
    }

    bool logical_and(Frame &frame, Value *out, std::string *error)
    {
        Value left;
        if (!bitwise_or(frame, &left, error))
            return false;
        while (true)
        {
            skip_space(frame.body, &frame.at);
            if (frame.at + 1 >= frame.body.size() || frame.body[frame.at] != '&' ||
                frame.body[frame.at + 1] != '&')
                break;
            frame.at += 2;
            if (!truthy(left))
            {
                skip_to_statement_end(frame);
                break;
            }
            if (!bitwise_or(frame, &left, error))
                return false;
        }
        *out = std::move(left);
        return true;
    }

    bool bitwise_or(Frame &frame, Value *out, std::string *error)
    {
        Value left;
        if (!bitwise_xor(frame, &left, error))
            return false;
        while (true)
        {
            skip_space(frame.body, &frame.at);
            if (frame.at >= frame.body.size() || frame.body[frame.at] != '|')
                break;
            if (frame.at + 1 < frame.body.size() && frame.body[frame.at + 1] == '|')
                break;
            ++frame.at;
            Value right;
            if (!bitwise_xor(frame, &right, error))
                return false;
            if (!bitwise(&left, right, '|', error))
                return false;
        }
        *out = std::move(left);
        return true;
    }

    bool bitwise_xor(Frame &frame, Value *out, std::string *error)
    {
        Value left;
        if (!bitwise_and(frame, &left, error))
            return false;
        while (true)
        {
            skip_space(frame.body, &frame.at);
            if (frame.at >= frame.body.size() || frame.body[frame.at] != '^')
                break;
            ++frame.at;
            Value right;
            if (!bitwise_and(frame, &right, error))
                return false;
            if (!bitwise(&left, right, '^', error))
                return false;
        }
        *out = std::move(left);
        return true;
    }

    bool bitwise_and(Frame &frame, Value *out, std::string *error)
    {
        Value left;
        if (!equality(frame, &left, error))
            return false;
        while (true)
        {
            skip_space(frame.body, &frame.at);
            if (frame.at >= frame.body.size() || frame.body[frame.at] != '&')
                break;
            if (frame.at + 1 < frame.body.size() && frame.body[frame.at + 1] == '&')
                break;
            ++frame.at;
            Value right;
            if (!equality(frame, &right, error))
                return false;
            if (!bitwise(&left, right, '&', error))
                return false;
        }
        *out = std::move(left);
        return true;
    }

    bool bitwise(Value *left, const Value &right, char op, std::string *error)
    {
        if (left->kind != Value::Kind::number || right.kind != Value::Kind::number)
        {
            *error = std::string("unsupported operator '") + op + "'";
            return false;
        }
        const long long a = static_cast<long long>(left->number);
        const long long b = static_cast<long long>(right.number);
        const long long value = op == '|' ? (a | b) : (op == '^' ? (a ^ b) : (a & b));
        left->number = static_cast<double>(value);
        return true;
    }

    bool equality(Frame &frame, Value *out, std::string *error)
    {
        Value left;
        if (!relational(frame, &left, error))
            return false;
        while (true)
        {
            skip_space(frame.body, &frame.at);
            if (frame.at + 1 >= frame.body.size())
                break;
            const char a = frame.body[frame.at];
            const char b = frame.body[frame.at + 1];
            const bool is_eq = a == '=' && b == '=';
            const bool is_ne = a == '!' && b == '=';
            if (!is_eq && !is_ne)
                break;
            frame.at += 2;
            if (frame.at < frame.body.size() && frame.body[frame.at] == '=')
                ++frame.at;
            Value right;
            if (!relational(frame, &right, error))
                return false;
            const bool same = loose_equal(left, right);
            left = number_value(is_eq == same ? 1.0 : 0.0);
        }
        *out = std::move(left);
        return true;
    }

    static bool loose_equal(const Value &left, const Value &right)
    {
        if (left.kind == Value::Kind::number && right.kind == Value::Kind::number)
            return left.number == right.number;
        if (left.kind == Value::Kind::string && right.kind == Value::Kind::string)
            return left.text == right.text;
        return left.kind == right.kind;
    }

    bool relational(Frame &frame, Value *out, std::string *error)
    {
        Value left;
        if (!shift(frame, &left, error))
            return false;
        while (true)
        {
            skip_space(frame.body, &frame.at);
            if (frame.at >= frame.body.size())
                break;
            const char op = frame.body[frame.at];
            if (op != '<' && op != '>')
                break;
            if (frame.at + 1 < frame.body.size() && frame.body[frame.at + 1] == op)
                break;
            bool inclusive = false;
            ++frame.at;
            if (frame.at < frame.body.size() && frame.body[frame.at] == '=')
            {
                inclusive = true;
                ++frame.at;
            }
            Value right;
            if (!shift(frame, &right, error))
                return false;
            if (left.kind != Value::Kind::number || right.kind != Value::Kind::number)
            {
                *error = std::string("unsupported operator '") + op + "'";
                return false;
            }
            const bool value =
                op == '<' ? (inclusive ? left.number <= right.number : left.number < right.number)
                          : (inclusive ? left.number >= right.number : left.number > right.number);
            left = number_value(value ? 1.0 : 0.0);
        }
        *out = std::move(left);
        return true;
    }

    bool shift(Frame &frame, Value *out, std::string *error)
    {
        Value left;
        if (!additive(frame, &left, error))
            return false;
        while (true)
        {
            skip_space(frame.body, &frame.at);
            if (frame.at + 1 >= frame.body.size())
                break;
            const char a = frame.body[frame.at];
            const char b = frame.body[frame.at + 1];
            if ((a != '<' && a != '>') || b != a)
                break;
            frame.at += 2;
            Value right;
            if (!additive(frame, &right, error))
                return false;
            if (left.kind != Value::Kind::number || right.kind != Value::Kind::number)
            {
                *error = "unsupported shift";
                return false;
            }
            const long long a0 = static_cast<long long>(left.number);
            const long long b0 = static_cast<long long>(right.number) & 31;
            left.number = static_cast<double>(a == '<' ? (a0 << b0) : (a0 >> b0));
        }
        *out = std::move(left);
        return true;
    }

    bool additive(Frame &frame, Value *out, std::string *error)
    {
        Value left;
        if (!multiplicative(frame, &left, error))
            return false;
        while (true)
        {
            skip_space(frame.body, &frame.at);
            if (frame.at >= frame.body.size())
                break;
            const char op = frame.body[frame.at];
            if (op != '+' && op != '-')
                break;
            ++frame.at;
            Value right;
            if (!multiplicative(frame, &right, error))
                return false;
            if (left.kind == Value::Kind::string && op == '+')
                left.text += right.kind == Value::Kind::string ? right.text : std::string();
            else if (left.kind == Value::Kind::number && right.kind == Value::Kind::number)
                left.number = op == '+' ? left.number + right.number : left.number - right.number;
            else
            {
                *error = std::string("unsupported operator '") + op + "'";
                return false;
            }
        }
        *out = std::move(left);
        return true;
    }

    bool multiplicative(Frame &frame, Value *out, std::string *error)
    {
        Value left;
        if (!unary(frame, &left, error))
            return false;
        while (true)
        {
            skip_space(frame.body, &frame.at);
            if (frame.at >= frame.body.size())
                break;
            const char op = frame.body[frame.at];
            if (op != '*' && op != '/' && op != '%')
                break;
            ++frame.at;
            Value right;
            if (!unary(frame, &right, error))
                return false;
            if (left.kind != Value::Kind::number || right.kind != Value::Kind::number)
            {
                *error = std::string("unsupported operator '") + op + "'";
                return false;
            }
            if (op == '*')
                left.number *= right.number;
            else if (op == '/')
                left.number = right.number == 0.0 ? 0.0 : left.number / right.number;
            else
            {
                const long long divisor = static_cast<long long>(right.number);
                left.number =
                    divisor == 0
                        ? 0.0
                        : static_cast<double>(static_cast<long long>(left.number) % divisor);
            }
        }
        *out = std::move(left);
        return true;
    }

    bool unary(Frame &frame, Value *out, std::string *error)
    {
        skip_space(frame.body, &frame.at);
        if (frame.at < frame.body.size() && frame.body[frame.at] == '!')
        {
            if (frame.at + 1 < frame.body.size() && frame.body[frame.at + 1] == '=')
            {
                // "!=" belongs to the equality level; an operand cannot start here.
                *error = "unsupported expression near: !=";
                return false;
            }
            ++frame.at;
            Value value;
            if (!unary(frame, &value, error))
                return false;
            *out = number_value(truthy(value) ? 0.0 : 1.0);
            return true;
        }
        if (frame.at < frame.body.size() && frame.body[frame.at] == '+')
        {
            ++frame.at;
            Value value;
            if (!unary(frame, &value, error))
                return false;
            if (value.kind != Value::Kind::number)
            {
                *error = "unsupported unary '+'";
                return false;
            }
            *out = std::move(value);
            return true;
        }
        if (frame.at < frame.body.size() && frame.body[frame.at] == '-')
        {
            ++frame.at;
            Value value;
            if (!unary(frame, &value, error))
                return false;
            if (value.kind != Value::Kind::number)
            {
                *error = "unsupported unary '-'";
                return false;
            }
            value.number = -value.number;
            *out = std::move(value);
            return true;
        }
        return postfix(frame, out, error);
    }

    bool postfix(Frame &frame, Value *out, std::string *error)
    {
        if (!primary(frame, out, error))
            return false;
        while (true)
        {
            skip_space(frame.body, &frame.at);
            if (frame.at >= frame.body.size())
                break;
            const char ch = frame.body[frame.at];
            if (ch == '.')
            {
                ++frame.at;
                skip_space(frame.body, &frame.at);
                if (frame.at >= frame.body.size() || !ident_start(frame.body[frame.at]))
                {
                    *error = "unsupported member access";
                    return false;
                }
                const std::size_t begin = frame.at;
                while (frame.at < frame.body.size() && ident_char(frame.body[frame.at]))
                    ++frame.at;
                const std::string member = frame.body.substr(begin, frame.at - begin);
                std::size_t probe = frame.at;
                skip_space(frame.body, &probe);
                const bool called = probe < frame.body.size() && frame.body[probe] == '(';
                if (member == "length" && !called)
                {
                    frame.at = probe;
                    if (out->kind == Value::Kind::array)
                        *out = number_value(static_cast<double>(out->array->size()));
                    else if (out->kind == Value::Kind::string)
                        *out = number_value(static_cast<double>(out->text.size()));
                    else
                    {
                        *error = "unsupported .length";
                        return false;
                    }
                    continue;
                }
                if (!called)
                {
                    *error = "unsupported member access: ." + member;
                    return false;
                }
                frame.at = probe;
                std::vector<Value> args;
                if (!call_args(frame, &args, error))
                    return false;
                Value base = *out;
                if (!apply_method(&base, member, args, out, error))
                    return false;
                continue;
            }
            if (ch == '[')
            {
                ++frame.at;
                Value index;
                if (!expression(frame, &index, error))
                    return false;
                skip_space(frame.body, &frame.at);
                if (frame.at >= frame.body.size() || frame.body[frame.at] != ']')
                {
                    *error = "unbalanced [ in expression";
                    return false;
                }
                ++frame.at;
                Value base = *out;
                std::size_t probe = frame.at;
                skip_space(frame.body, &probe);
                if (probe < frame.body.size() && frame.body[probe] == '(')
                {
                    // base["name"](...) -- a method or a table entry.
                    if (index.kind != Value::Kind::string)
                    {
                        *error = "unsupported: computed call needs a string member";
                        return false;
                    }
                    frame.at = probe;
                    std::vector<Value> args;
                    if (!call_args(frame, &args, error))
                        return false;
                    if (!apply_method(&base, index.text, args, out, error))
                        return false;
                    continue;
                }
                if (!element(base, index, out, error))
                    return false;
                continue;
            }
            break;
        }
        return true;
    }

    bool primary(Frame &frame, Value *out, std::string *error)
    {
        if (!tick(error))
            return false;
        skip_space(frame.body, &frame.at);
        if (frame.at >= frame.body.size())
        {
            *error = "unexpected end of expression";
            return false;
        }
        const char ch = frame.body[frame.at];
        if (ch == '(')
        {
            ++frame.at;
            if (!expression(frame, out, error))
                return false;
            skip_space(frame.body, &frame.at);
            if (frame.at >= frame.body.size() || frame.body[frame.at] != ')')
            {
                *error = "unbalanced ( in expression";
                return false;
            }
            ++frame.at;
            return true;
        }
        if (ch == '"' || ch == '\'')
        {
            const std::size_t begin = frame.at + 1;
            const std::size_t end = skip_string(frame.body, frame.at);
            frame.at = end;
            std::string text;
            for (std::size_t index = begin; index + 1 < end; ++index)
            {
                if (frame.body[index] == '\\' && index + 2 < end)
                {
                    ++index;
                    const char esc = frame.body[index];
                    text += esc == 'n' ? '\n' : (esc == 't' ? '\t' : esc);
                }
                else
                    text += frame.body[index];
            }
            *out = string_value(std::move(text));
            return true;
        }
        if ((ch >= '0' && ch <= '9') || ch == '.')
        {
            const std::size_t begin = frame.at;
            while (frame.at < frame.body.size() &&
                   ((frame.body[frame.at] >= '0' && frame.body[frame.at] <= '9') ||
                    frame.body[frame.at] == '.'))
                ++frame.at;
            const std::string text = frame.body.substr(begin, frame.at - begin);
            *out = number_value(std::strtod(text.c_str(), nullptr));
            return true;
        }
        if (ident_start(ch))
        {
            const std::size_t begin = frame.at;
            while (frame.at < frame.body.size() && ident_char(frame.body[frame.at]))
                ++frame.at;
            const std::string name = frame.body.substr(begin, frame.at - begin);
            if (name == "String")
            {
                *out = Value{};
                out->kind = Value::Kind::ctor;
                out->text = "String";
                return true;
            }
            return identifier(frame, name, out, error);
        }
        *error = "unsupported expression near: " + frame.body.substr(frame.at, 24);
        return false;
    }

    bool identifier(Frame &frame, const std::string &name, Value *out, std::string *error)
    {
        std::size_t probe = frame.at;
        skip_space(frame.body, &probe);
        const char next = probe < frame.body.size() ? frame.body[probe] : '\0';
        const auto known = frame.vars.find(name);
        if (known != frame.vars.end())
        {
            *out = known->second;
            return true;
        }
        if (next == '(')
        {
            frame.at = probe;
            std::vector<Value> args;
            if (!call_args(frame, &args, error))
                return false;
            return call_script(name, args, out, error);
        }
        if (next == '.')
        {
            std::size_t member_at = probe + 1;
            skip_space(frame.body, &member_at);
            if (member_at >= frame.body.size() || !ident_start(frame.body[member_at]))
            {
                *error = "unsupported member access";
                return false;
            }
            const std::size_t begin = member_at;
            while (member_at < frame.body.size() && ident_char(frame.body[member_at]))
                ++member_at;
            const std::string member = frame.body.substr(begin, member_at - begin);
            std::size_t call_at = member_at;
            skip_space(frame.body, &call_at);
            if (call_at >= frame.body.size() || frame.body[call_at] != '(')
            {
                *error = "unknown identifier: " + name;
                return false;
            }
            frame.at = call_at;
            std::vector<Value> args;
            if (!call_args(frame, &args, error))
                return false;
            return call_script(name + "." + member, args, out, error);
        }
        *error = "unknown identifier: " + name;
        return false;
    }

    bool call_script(const std::string &name, const std::vector<Value> &args, Value *out,
                     std::string *error)
    {
        if (name == "encodeURIComponent")
        {
            if (args.size() != 1 || args[0].kind != Value::Kind::string)
            {
                *error = "unsupported: encodeURIComponent arguments";
                return false;
            }
            *out = string_value(encode_uri_component(args[0].text));
            return true;
        }
        if (name == "decodeURIComponent" || name == "unescape")
        {
            if (args.size() != 1 || args[0].kind != Value::Kind::string)
            {
                *error = "unsupported: " + name + " arguments";
                return false;
            }
            *out = string_value(percent_decode(args[0].text));
            return true;
        }
        if (name == "escape")
        {
            if (args.size() != 1 || args[0].kind != Value::Kind::string)
            {
                *error = "unsupported: escape arguments";
                return false;
            }
            static const char hex[] = "0123456789ABCDEF";
            std::string text;
            for (char raw : args[0].text)
            {
                const unsigned char ch = static_cast<unsigned char>(raw);
                const bool plain = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                   (ch >= '0' && ch <= '9') || ch == '@' || ch == '*' ||
                                   ch == '_' || ch == '+' || ch == '-' || ch == '.' || ch == '/';
                if (plain)
                    text += static_cast<char>(ch);
                else
                {
                    text += '%';
                    text += hex[(ch >> 4) & 0x0f];
                    text += hex[ch & 0x0f];
                }
            }
            *out = string_value(std::move(text));
            return true;
        }
        if (name == "parseInt")
        {
            if (args.empty() || args[0].kind != Value::Kind::string)
            {
                *error = "unsupported: parseInt arguments";
                return false;
            }
            *out = number_value(std::strtod(args[0].text.c_str(), nullptr));
            return true;
        }
        if (name == "String")
        {
            if (args.empty())
            {
                *out = string_value(std::string());
                return true;
            }
            if (args[0].kind == Value::Kind::number)
            {
                char buffer[32];
                std::snprintf(buffer, sizeof(buffer), "%g", args[0].number);
                *out = string_value(buffer);
                return true;
            }
            *out = string_value(args[0].text);
            return true;
        }
        const JsFunction *function = script_->find(name);
        if (function == nullptr)
        {
            *error = "unknown function: " + name;
            return false;
        }
        return run(*function, args, out, error);
    }

    bool call_args(Frame &frame, std::vector<Value> *args, std::string *error)
    {
        ++frame.at; // '('
        skip_space(frame.body, &frame.at);
        if (frame.at < frame.body.size() && frame.body[frame.at] == ')')
        {
            ++frame.at;
            return true;
        }
        while (true)
        {
            Value value;
            if (!expression(frame, &value, error))
                return false;
            args->push_back(std::move(value));
            skip_space(frame.body, &frame.at);
            if (frame.at >= frame.body.size())
            {
                *error = "unbalanced ( in argument list";
                return false;
            }
            if (frame.body[frame.at] == ',')
            {
                ++frame.at;
                continue;
            }
            if (frame.body[frame.at] == ')')
            {
                ++frame.at;
                return true;
            }
            *error = "malformed argument list";
            return false;
        }
    }

    bool element(const Value &base, const Value &index, Value *out, std::string *error)
    {
        if (base.kind == Value::Kind::words)
        {
            if (index.kind != Value::Kind::number)
            {
                *error = "unsupported: string-table index is not a number";
                return false;
            }
            const long long at = static_cast<long long>(index.number);
            if (at < 0 || static_cast<std::size_t>(at) >= base.words->size())
            {
                *error = "string-table index out of range";
                return false;
            }
            *out = string_value((*base.words)[static_cast<std::size_t>(at)]);
            return true;
        }
        if (base.kind == Value::Kind::table)
        {
            if (index.kind != Value::Kind::string)
            {
                *error = "unsupported: table index is not a string";
                return false;
            }
            const auto member = base.table->find(index.text);
            if (member == base.table->end())
            {
                *error = "unknown table entry: " + index.text;
                return false;
            }
            Value value;
            value.kind = Value::Kind::fn;
            value.fn = std::make_shared<JsFunction>(member->second);
            *out = std::move(value);
            return true;
        }
        if (index.kind == Value::Kind::string)
        {
            if (index.text == "length" &&
                (base.kind == Value::Kind::array || base.kind == Value::Kind::string))
            {
                *out = number_value(static_cast<double>(
                    base.kind == Value::Kind::array ? base.array->size() : base.text.size()));
                return true;
            }
            *error = "unsupported: string index on a sequence";
            return false;
        }
        if (index.kind != Value::Kind::number)
        {
            *error = "unsupported: index is not a number";
            return false;
        }
        const long long at = static_cast<long long>(index.number);
        char ch = '\0';
        if (base.kind == Value::Kind::array)
        {
            if (at < 0 || static_cast<std::size_t>(at) >= base.array->size())
            {
                *error = "index out of range";
                return false;
            }
            ch = (*base.array)[static_cast<std::size_t>(at)];
        }
        else if (base.kind == Value::Kind::string)
        {
            if (at < 0 || static_cast<std::size_t>(at) >= base.text.size())
            {
                *error = "index out of range";
                return false;
            }
            ch = base.text[static_cast<std::size_t>(at)];
        }
        else
        {
            *error = "unsupported: index access on a non-sequence";
            return false;
        }
        *out = string_value(std::string(1, ch));
        return true;
    }

    bool apply_method(const Value *base, const std::string &name, const std::vector<Value> &args,
                      Value *out, std::string *error)
    {
        if (base->kind == Value::Kind::fn)
        {
            if (name == "call")
                return run(*base->fn, args, out, error);
            *error = "unsupported operation on a function: ." + name;
            return false;
        }
        if (base->kind == Value::Kind::table)
        {
            const auto member = base->table->find(name);
            if (member == base->table->end())
            {
                *error = "unknown table entry: " + name;
                return false;
            }
            return run(member->second, args, out, error);
        }
        if (base->kind == Value::Kind::ctor)
        {
            if (name == "fromCharCode")
            {
                std::string text;
                for (const Value &value : args)
                {
                    if (value.kind != Value::Kind::number)
                    {
                        *error = "unsupported: String.fromCharCode expects numbers";
                        return false;
                    }
                    text += static_cast<char>(static_cast<int>(value.number) & 0xff);
                }
                *out = string_value(std::move(text));
                return true;
            }
            *error = "unsupported operation: String." + name;
            return false;
        }

        if (base->kind == Value::Kind::array)
        {
            if (name == "reverse")
            {
                std::reverse(base->array->begin(), base->array->end());
                *out = *base;
                return true;
            }
            if (name == "join")
            {
                std::string separator = ",";
                if (!args.empty())
                {
                    if (args[0].kind != Value::Kind::string)
                    {
                        *error = "unsupported: join separator";
                        return false;
                    }
                    separator = args[0].text;
                }
                std::string text;
                for (std::size_t index = 0; index < base->array->size(); ++index)
                {
                    if (index > 0)
                        text += separator;
                    text += (*base->array)[index];
                }
                *out = string_value(std::move(text));
                return true;
            }
            if (name == "slice" || name == "splice")
            {
                if (args.empty() || args[0].kind != Value::Kind::number)
                {
                    *error = "unsupported: " + name + " arguments";
                    return false;
                }
                const long long size = static_cast<long long>(base->array->size());
                long long start = static_cast<long long>(args[0].number);
                if (start < 0)
                    start += size;
                if (start < 0)
                    start = 0;
                if (start > size)
                    start = size;
                if (name == "splice")
                {
                    long long count = size - start;
                    if (args.size() > 1 && args[1].kind == Value::Kind::number)
                        count = static_cast<long long>(args[1].number);
                    if (count < 0)
                        count = 0;
                    if (count > size - start)
                        count = size - start;
                    std::string removed = base->array->substr(static_cast<std::size_t>(start),
                                                              static_cast<std::size_t>(count));
                    base->array->erase(static_cast<std::size_t>(start),
                                       static_cast<std::size_t>(count));
                    *out = array_value(std::move(removed));
                    return true;
                }
                long long end = size;
                if (args.size() > 1 && args[1].kind == Value::Kind::number)
                    end = static_cast<long long>(args[1].number);
                if (end < 0)
                    end += size;
                if (end < start)
                    end = start;
                if (end > size)
                    end = size;
                *out = array_value(base->array->substr(static_cast<std::size_t>(start),
                                                       static_cast<std::size_t>(end - start)));
                return true;
            }
            if (name == "push")
            {
                for (const Value &value : args)
                {
                    if (value.kind == Value::Kind::string && !value.text.empty())
                        base->array->push_back(value.text[0]);
                    else if (value.kind == Value::Kind::number)
                        base->array->push_back(
                            static_cast<char>(static_cast<int>(value.number) & 0xff));
                    else
                    {
                        *error = "unsupported: push value";
                        return false;
                    }
                }
                *out = number_value(static_cast<double>(base->array->size()));
                return true;
            }
            *error = "unsupported operation: ." + name;
            return false;
        }

        if (base->kind == Value::Kind::string)
        {
            if (name == "split")
            {
                if (args.size() != 1 || args[0].kind != Value::Kind::string ||
                    !args[0].text.empty())
                {
                    *error = "unsupported operation: .split(";
                    return false;
                }
                *out = array_value(base->text);
                return true;
            }
            if (name == "charCodeAt")
            {
                if (args.empty() || args[0].kind != Value::Kind::number)
                {
                    *error = "unsupported operation: .charCodeAt(";
                    return false;
                }
                const long long at = static_cast<long long>(args[0].number);
                if (at < 0 || static_cast<std::size_t>(at) >= base->text.size())
                {
                    *error = "charCodeAt out of range";
                    return false;
                }
                *out = number_value(static_cast<double>(
                    static_cast<unsigned char>(base->text[static_cast<std::size_t>(at)])));
                return true;
            }
            if (name == "charAt")
            {
                if (args.empty() || args[0].kind != Value::Kind::number)
                {
                    *error = "unsupported operation: .charAt(";
                    return false;
                }
                const long long at = static_cast<long long>(args[0].number);
                *out = string_value(at < 0 || static_cast<std::size_t>(at) >= base->text.size()
                                        ? std::string()
                                        : base->text.substr(static_cast<std::size_t>(at), 1));
                return true;
            }
            if (name == "replace")
            {
                if (args.size() < 2 || args[0].kind != Value::Kind::string ||
                    args[1].kind != Value::Kind::string)
                {
                    *error = "unsupported operation: .replace(";
                    return false;
                }
                const std::size_t at = base->text.find(args[0].text);
                std::string text = base->text;
                if (at != std::string::npos)
                    text.replace(at, args[0].text.size(), args[1].text);
                *out = string_value(std::move(text));
                return true;
            }
            if (name == "indexOf")
            {
                if (args.empty() || args[0].kind != Value::Kind::string)
                {
                    *error = "unsupported operation: .indexOf(";
                    return false;
                }
                const std::size_t at = base->text.find(args[0].text);
                *out = number_value(at == std::string::npos ? -1.0 : static_cast<double>(at));
                return true;
            }
            if (name == "slice" || name == "substring" || name == "substr")
            {
                if (args.empty() || args[0].kind != Value::Kind::number)
                {
                    *error = "unsupported operation: ." + name + "(";
                    return false;
                }
                const long long size = static_cast<long long>(base->text.size());
                long long start = static_cast<long long>(args[0].number);
                if (start < 0)
                    start += size;
                if (start < 0)
                    start = 0;
                if (start > size)
                    start = size;
                long long end = size;
                if (args.size() > 1 && args[1].kind == Value::Kind::number)
                {
                    if (name == "substr")
                        end = start + static_cast<long long>(args[1].number);
                    else
                        end = static_cast<long long>(args[1].number);
                }
                if (end < 0)
                    end += size;
                if (end < start)
                    end = start;
                if (end > size)
                    end = size;
                *out = string_value(base->text.substr(static_cast<std::size_t>(start),
                                                      static_cast<std::size_t>(end - start)));
                return true;
            }
            if (name == "toString" || name == "valueOf")
            {
                *out = *base;
                return true;
            }
            *error = "unsupported operation: ." + name;
            return false;
        }

        *error = "unsupported operation: ." + name + " on a number";
        return false;
    }

    bool assign_target(Frame &frame, const std::string &target, const Value &value,
                       std::string *error)
    {
        const std::string_view text = trim(target);
        const std::size_t bracket = text.find('[');
        if (bracket == std::string_view::npos)
        {
            if (!is_identifier(text))
            {
                *error = "unsupported assignment target: " + std::string(text);
                return false;
            }
            frame.vars[std::string(text)] = value;
            return true;
        }
        if (text.empty() || text.back() != ']')
        {
            *error = "unsupported assignment target: " + std::string(text);
            return false;
        }
        const std::string_view name = trim(text.substr(0, bracket));
        const std::string_view index_text = text.substr(bracket + 1, text.size() - bracket - 2);
        if (!is_identifier(name))
        {
            *error = "unsupported assignment target: " + std::string(text);
            return false;
        }
        const auto known = frame.vars.find(std::string(name));
        if (known == frame.vars.end() || known->second.kind != Value::Kind::array)
        {
            *error = "index assignment to a non-array: " + std::string(name);
            return false;
        }
        double index = 0.0;
        if (!eval_number(frame, index_text, &index, error))
            return false;
        const long long at = static_cast<long long>(index);
        if (at < 0 || static_cast<std::size_t>(at) >= known->second.array->size())
        {
            *error = "index out of range in assignment";
            return false;
        }
        char ch = '\0';
        if (value.kind == Value::Kind::string && !value.text.empty())
            ch = value.text[0];
        else if (value.kind == Value::Kind::number)
            ch = static_cast<char>(static_cast<int>(value.number) & 0xff);
        else
        {
            *error = "unsupported assigned value";
            return false;
        }
        (*known->second.array)[static_cast<std::size_t>(at)] = ch;
        return true;
    }

    bool eval_value(Frame &frame, std::string_view text, Value *out, std::string *error)
    {
        const std::string owned(text);
        Frame sub{owned, 0, frame.vars};
        return expression(sub, out, error);
    }

    bool eval_number(Frame &frame, std::string_view text, double *out, std::string *error)
    {
        Value value;
        if (!eval_value(frame, text, &value, error))
            return false;
        if (value.kind != Value::Kind::number)
        {
            *error = "expected a number";
            return false;
        }
        *out = value.number;
        return true;
    }

    static bool keyword(const std::string &text, std::size_t at, const char *word)
    {
        const std::size_t length = std::strlen(word);
        if (text.compare(at, length, word) != 0)
            return false;
        if (at + length < text.size() && ident_char(text[at + length]))
            return false;
        return true;
    }

    static std::size_t find_assign(const std::string &body, std::size_t from)
    {
        int depth = 0;
        for (std::size_t index = from; index < body.size(); ++index)
        {
            const char ch = body[index];
            if (ch == '"' || ch == '\'' || ch == '`')
            {
                index = skip_string(body, index) - 1;
                continue;
            }
            if (ch == '(' || ch == '[' || ch == '{')
                ++depth;
            else if (ch == ')' || ch == ']' || ch == '}')
            {
                if (depth == 0)
                    return std::string::npos;
                --depth;
            }
            else if (ch == ';' && depth == 0)
                return std::string::npos;
            else if (ch == '=' && depth == 0)
            {
                const char before = index > from ? body[index - 1] : '\0';
                const char after = index + 1 < body.size() ? body[index + 1] : '\0';
                if (before == '=' || before == '!' || before == '<' || before == '>' ||
                    after == '=' || after == '>')
                    continue;
                return index;
            }
        }
        return std::string::npos;
    }

    static std::vector<std::string_view> split_commas(std::string_view text)
    {
        std::vector<std::string_view> parts;
        int depth = 0;
        std::size_t begin = 0;
        for (std::size_t index = 0; index < text.size(); ++index)
        {
            const char ch = text[index];
            if (ch == '"' || ch == '\'' || ch == '`')
            {
                const char quote = ch;
                ++index;
                while (index < text.size() && text[index] != quote)
                {
                    if (text[index] == '\\')
                        ++index;
                    ++index;
                }
                continue;
            }
            if (ch == '(' || ch == '[' || ch == '{')
                ++depth;
            else if (ch == ')' || ch == ']' || ch == '}')
                --depth;
            else if (ch == ',' && depth == 0)
            {
                parts.push_back(trim(text.substr(begin, index - begin)));
                begin = index + 1;
            }
        }
        parts.push_back(trim(text.substr(begin)));
        return parts;
    }
};

// ---- the anchors ----------------------------------------------------------------------------

std::size_t find_setter(const std::string &js, const char *name)
{
    const std::string double_quoted = std::string("set(\"") + name + "\"";
    const std::string single_quoted = std::string("set('") + name + "'";
    const std::size_t a = js.find(double_quoted);
    const std::size_t b = js.find(single_quoted);
    if (a == std::string::npos)
        return b;
    if (b == std::string::npos)
        return a;
    return a < b ? a : b;
}

// The argument expression of "set(name, <expression>)".
bool setter_expression(const std::string &js, const char *name, std::string *out)
{
    const std::size_t at = find_setter(js, name);
    if (at == std::string::npos)
        return false;
    std::size_t index = js.find('(', at);
    if (index == std::string::npos)
        return false;
    ++index;
    skip_space(js, &index);
    if (index >= js.size() || (js[index] != '"' && js[index] != '\''))
        return false;
    index = skip_string(js, index);
    skip_space(js, &index);
    if (index >= js.size() || js[index] != ',')
        return false;
    ++index;
    const std::size_t start = index;
    int depth = 1;
    for (; index < js.size(); ++index)
    {
        const char ch = js[index];
        if (ch == '"' || ch == '\'' || ch == '`')
        {
            index = skip_string(js, index) - 1;
            continue;
        }
        if (ch == '(')
            ++depth;
        else if (ch == ')')
        {
            --depth;
            if (depth == 0)
                break;
        }
    }
    if (index >= js.size())
        return false;
    *out = std::string(trim(std::string_view(js).substr(start, index - start)));
    return true;
}

std::vector<std::string> identifiers(std::string_view text)
{
    std::vector<std::string> out;
    std::size_t index = 0;
    while (index < text.size())
    {
        if (ident_start(text[index]))
        {
            const std::size_t begin = index;
            while (index < text.size() && ident_char(text[index]))
                ++index;
            out.emplace_back(text.substr(begin, index - begin));
        }
        else if (text[index] == '"' || text[index] == '\'' || text[index] == '`')
        {
            const char quote = text[index];
            ++index;
            while (index < text.size() && text[index] != quote)
            {
                if (text[index] == '\\')
                    ++index;
                ++index;
            }
            ++index;
        }
        else
            ++index;
    }
    return out;
}

// ---- structural extraction ------------------------------------------------------------------

// The player's own ";".split(";") string table: every obfuscated helper indexes
// it, so the interpreter has to be able to read it.
bool parse_string_table(const std::string &js, std::string *name, std::vector<std::string> *words)
{
    static const char kSplit[] = ".split(\";\")";
    std::size_t at = 0;
    while ((at = js.find("='", at)) != std::string::npos)
    {
        const std::size_t quote = at + 1;
        const std::size_t end = skip_string(js, quote);
        if (js.compare(end, std::strlen(kSplit), kSplit) == 0)
        {
            std::size_t begin = at;
            while (begin > 0 && ident_char(js[begin - 1]))
                --begin;
            if (begin == at)
            {
                at = end;
                continue;
            }
            *name = js.substr(begin, at - begin);
            const std::string_view content =
                std::string_view(js).substr(quote + 1, end - 1 - quote - 1);
            words->clear();
            std::size_t part = 0;
            while (part <= content.size())
            {
                const std::size_t semi = content.find(';', part);
                const std::size_t length =
                    semi == std::string_view::npos ? content.size() - part : semi - part;
                std::string word;
                for (std::size_t index = part; index < part + length; ++index)
                {
                    if (content[index] == '\\' && index + 1 < part + length)
                        ++index;
                    word += content[index];
                }
                words->push_back(std::move(word));
                if (semi == std::string_view::npos)
                    break;
                part = semi + 1;
            }
            return true;
        }
        at = end;
    }
    return false;
}

// Object literals whose members are small helper functions, e.g. the player's
// own operation table "{dl:function(a,b){a.splice(0,b)},...}".
bool parse_object_tables(const std::string &js, std::map<std::string, Value> *globals)
{
    std::size_t at = 0;
    while ((at = js.find("={", at)) != std::string::npos)
    {
        std::size_t begin = at;
        while (begin > 0 && ident_char(js[begin - 1]))
            --begin;
        if (begin == at)
        {
            at += 2;
            continue;
        }
        const std::string name = js.substr(begin, at - begin);
        const std::size_t open = at + 1;
        const std::size_t close = skip_brackets(js, open);
        if (close == std::string::npos)
        {
            at += 2;
            continue;
        }
        const std::string body = js.substr(open + 1, close - open - 2);
        if (body.size() <= 4000 && body.find("function") != std::string::npos &&
            body.find("[0]") != std::string::npos && body.find('%') != std::string::npos)
        {
            auto table = std::make_shared<std::map<std::string, JsFunction>>();
            std::size_t fn = 0;
            while ((fn = body.find("function", fn)) != std::string::npos)
            {
                std::size_t colon = fn;
                while (colon > 0 && is_space(body[colon - 1]))
                    --colon;
                if (colon == 0 || body[colon - 1] != ':')
                {
                    fn += std::strlen("function");
                    continue;
                }
                --colon;
                while (colon > 0 && is_space(body[colon - 1]))
                    --colon;
                std::size_t key_begin = colon;
                while (key_begin > 0 && ident_char(body[key_begin - 1]))
                    --key_begin;
                if (key_begin == colon)
                {
                    fn += std::strlen("function");
                    continue;
                }
                const std::string key = body.substr(key_begin, colon - key_begin);
                std::string parsed;
                JsFunction function;
                std::size_t end = 0;
                if (parse_function(body, fn, &parsed, &function, &end))
                {
                    (*table)[key] = std::move(function);
                    fn = end;
                    continue;
                }
                fn += std::strlen("function");
            }
            if (!table->empty())
            {
                Value value;
                value.kind = Value::Kind::table;
                value.table = std::move(table);
                (*globals)[name] = std::move(value);
            }
        }
        at = close;
    }
    return true;
}

// A helper that drives one of those tables: the player's own signature or
// throttle transform, rather than a lookup table.
bool find_table_helper(const std::string &js, const std::map<std::string, Value> &globals,
                       std::string *name, JsFunction *out)
{
    std::size_t at = 0;
    while ((at = js.find("=function(", at)) != std::string::npos)
    {
        std::size_t begin = at;
        while (begin > 0 && ident_char(js[begin - 1]))
            --begin;
        if (begin == at)
        {
            at += 1;
            continue;
        }
        const std::string candidate = js.substr(begin, at - begin);
        std::string parsed;
        JsFunction function;
        std::size_t end = 0;
        if (!parse_function(js, at + 1, &parsed, &function, &end))
        {
            at += 1;
            continue;
        }
        at = end;
        if (function.body.size() > kMaxFunctionBytes)
            continue;
        bool uses_table = false;
        for (const auto &entry : globals)
        {
            if (entry.second.kind != Value::Kind::table)
                continue;
            if (function.body.find(entry.first + "[") != std::string::npos)
            {
                uses_table = true;
                break;
            }
        }
        if (!uses_table)
            continue;
        if (function.body.find("split") == std::string::npos &&
            function.body.find("join") == std::string::npos &&
            function.body.find("e[") == std::string::npos)
            continue;
        *name = candidate;
        *out = std::move(function);
        return true;
    }
    return false;
}

// The (F, w) selector a call site passes to an obfuscated dispatcher, e.g.
// "We(2,3435,...)".
struct Selector
{
    double first = 0.0;
    double second = 0.0;
};

std::vector<Selector> find_selectors(const std::string &js, const std::string &name)
{
    std::vector<Selector> out;
    const std::string call = name + "(";
    std::size_t at = 0;
    while ((at = js.find(call, at)) != std::string::npos)
    {
        if (at > 0 && ident_char(js[at - 1]))
        {
            at += call.size();
            continue;
        }
        std::size_t index = at + call.size();
        skip_space(js, &index);
        const std::size_t first_begin = index;
        while (index < js.size() && ((js[index] >= '0' && js[index] <= '9') || js[index] == '-'))
            ++index;
        if (index == first_begin)
        {
            at += call.size();
            continue;
        }
        const double first =
            std::strtod(js.substr(first_begin, index - first_begin).c_str(), nullptr);
        skip_space(js, &index);
        if (index >= js.size() || js[index] != ',')
        {
            at += call.size();
            continue;
        }
        ++index;
        skip_space(js, &index);
        const std::size_t second_begin = index;
        while (index < js.size() && ((js[index] >= '0' && js[index] <= '9') || js[index] == '-'))
            ++index;
        if (index == second_begin)
        {
            at += call.size();
            continue;
        }
        Selector selector;
        selector.first = first;
        selector.second =
            std::strtod(js.substr(second_begin, index - second_begin).c_str(), nullptr);
        out.push_back(selector);
        at = index;
    }
    return out;
}

bool is_signature_helper(const JsFunction &function)
{
    const bool split_join = function.body.find(".split(") != std::string::npos &&
                            function.body.find(".join(") != std::string::npos;
    const bool chars = function.body.find("charCodeAt") != std::string::npos ||
                       function.body.find("fromCharCode") != std::string::npos;
    const bool loop = function.body.find("for(") != std::string::npos ||
                      function.body.find("for (") != std::string::npos ||
                      function.body.find("while(") != std::string::npos ||
                      function.body.find("while (") != std::string::npos;
    return split_join || chars || loop;
}

bool is_throttle_helper(const JsFunction &function)
{
    return function.body.find("fromCharCode") != std::string::npos || is_signature_helper(function);
}

bool extract_signature(const std::string &js, Script *script, JsFunction *out, bool allow_fallback)
{
    std::string expression;
    if (setter_expression(js, "signature", &expression))
    {
        for (const std::string &name : identifiers(expression))
        {
            const JsFunction *function = script->find(name);
            if (function != nullptr && is_signature_helper(*function))
            {
                *out = *function;
                return true;
            }
        }
        const std::size_t at = expression.find("function");
        if (at != std::string::npos)
        {
            std::string parsed;
            JsFunction function;
            std::size_t end = 0;
            if (parse_function(expression, at, &parsed, &function, &end) &&
                is_signature_helper(function))
            {
                *out = std::move(function);
                return true;
            }
        }
    }
    if (!allow_fallback)
        return false;
    // Fallback for a build whose setter names none of the helpers directly:
    // the last function that splits and joins is the signature one.
    std::size_t at = 0;
    bool found = false;
    while ((at = js.find("function", at)) != std::string::npos)
    {
        std::string parsed;
        JsFunction function;
        std::size_t end = 0;
        if (parse_function(js, at, &parsed, &function, &end) && is_signature_helper(function))
        {
            *out = std::move(function);
            found = true;
        }
        at += std::strlen("function");
    }
    return found;
}

// A call site that reads or writes the throttle parameter. A plain build
// writes it through a "set("n", ...)" setter; today's TV build only reads it,
// with "get("n")", and hands it to a VM. Either way a helper has to be reached
// from here to count as the throttle - one the cipher/signature setters call
// is not a throttle helper.
bool has_n_call_site(const std::string &js)
{
    if (find_setter(js, "n") != std::string::npos)
        return true;
    return js.find("get(\"n\"") != std::string::npos || js.find("get('n'") != std::string::npos;
}

// Provenance: the helper must be reachable from an n call site. The setter's
// own expression is the n call site, so a helper it names qualifies; there is
// no fallback that would borrow the signature helper.
bool extract_throttle(const std::string &js, Script *script, JsFunction *out, std::string *note)
{
    std::string expression;
    if (setter_expression(js, "n", &expression))
    {
        for (const std::string &name : identifiers(expression))
        {
            const JsFunction *function = script->find(name);
            if (function != nullptr && is_throttle_helper(*function))
            {
                *out = *function;
                return true;
            }
        }
        const std::size_t at = expression.find("function");
        if (at != std::string::npos)
        {
            std::string parsed;
            JsFunction function;
            std::size_t end = 0;
            if (parse_function(expression, at, &parsed, &function, &end) &&
                is_throttle_helper(function))
            {
                *out = std::move(function);
                return true;
            }
        }
        if (note != nullptr)
            *note = "the n setter names no helper this subset can run";
        return false;
    }
    // An n call site with nothing this subset can run behind it: today's TV
    // build reads the parameter with get("n") and drives an uninterpretable
    // VM. The throttle stays unsolved rather than reusing the signature helper.
    if (has_n_call_site(js) && note != nullptr)
        *note = "the player's n path is not a helper this subset can run (a VM behind "
                "get(\"n\"))";
    return false;
}

// Operations the subset does not implement. Naming one refuses the whole
// player JS, which is safer than guessing and playing a wrong URL.
bool find_unsupported(const std::string &body, std::string *snippet)
{
    static const char *const kBanned[] = {
        "switch(", "switch ",  "try{",      "try {",  "catch(",      "catch (",  "=>",
        ".pop(",   ".shift(",  ".unshift(", ".map(",  ".filter(",    ".reduce(", ".match(",
        ".test(",  ".concat(", ".apply(",   "Math.",  "parseFloat(", "eval(",    "new ",
        "delete ", "typeof ",  "throw ",    "yield ",
    };
    for (const char *token : kBanned)
    {
        if (body.find(token) != std::string::npos)
        {
            *snippet = token;
            return true;
        }
    }
    return false;
}

// ---- URL helpers ----------------------------------------------------------------------------

int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9')
        return ch - '0';
    if (ch >= 'a' && ch <= 'f')
        return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F')
        return ch - 'A' + 10;
    return -1;
}

std::string percent_decode(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (std::size_t index = 0; index < text.size(); ++index)
    {
        if (text[index] == '%' && index + 2 < text.size())
        {
            const int high = hex_value(text[index + 1]);
            const int low = hex_value(text[index + 2]);
            if (high >= 0 && low >= 0)
            {
                out += static_cast<char>((high << 4) | low);
                index += 2;
                continue;
            }
        }
        out += text[index];
    }
    return out;
}

// encodeURIComponent of the JS the player uses to build the final URL.
std::string encode_uri_component(std::string_view text)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(text.size());
    for (char raw : text)
    {
        const unsigned char ch = static_cast<unsigned char>(raw);
        const bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                                (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
                                ch == '!' || ch == '~' || ch == '*' || ch == '\'' || ch == '(' ||
                                ch == ')';
        if (unreserved)
            out += static_cast<char>(ch);
        else
        {
            out += '%';
            out += hex[(ch >> 4) & 0x0f];
            out += hex[ch & 0x0f];
        }
    }
    return out;
}

bool param_value(const std::string &url, std::string_view key, std::string *out)
{
    const std::size_t query = url.find('?');
    if (query == std::string::npos)
        return false;
    std::size_t at = query + 1;
    while (at <= url.size())
    {
        std::size_t end = url.find('&', at);
        if (end == std::string::npos)
            end = url.size();
        const std::size_t equals = url.find('=', at);
        if (equals != std::string::npos && equals < end &&
            std::string_view(url).substr(at, equals - at) == key)
        {
            *out = url.substr(equals + 1, end - equals - 1);
            return true;
        }
        if (end >= url.size())
            break;
        at = end + 1;
    }
    return false;
}

void set_param(std::string *url, std::string_view key, const std::string &value)
{
    const std::size_t query = url->find('?');
    if (query == std::string::npos)
    {
        *url += '?';
        *url += std::string(key);
        *url += '=';
        *url += value;
        return;
    }
    std::size_t at = query + 1;
    while (at <= url->size())
    {
        std::size_t end = url->find('&', at);
        if (end == std::string::npos)
            end = url->size();
        const std::size_t equals = url->find('=', at);
        if (equals != std::string::npos && equals < end &&
            std::string_view(*url).substr(at, equals - at) == key)
        {
            url->replace(equals + 1, end - equals - 1, value);
            return;
        }
        if (end >= url->size())
            break;
        at = end + 1;
    }
    *url += '&';
    *url += std::string(key);
    *url += '=';
    *url += value;
}

// Runs one extracted helper over a value, prepending the selector arguments an
// obfuscated dispatcher needs.
bool run_helper(Script *script, const std::map<std::string, Value> &globals,
                const JsFunction &function, const std::vector<Value> &prefix,
                const std::string &input, std::string *out, std::string *error)
{
    Interp interp(script);
    interp.set_globals(&globals);
    std::vector<Value> args = prefix;
    args.push_back(string_value(input));
    Value result;
    if (!interp.run(function, args, &result, error))
        return false;
    if (result.kind != Value::Kind::string)
    {
        *error = "helper returned a non-string";
        return false;
    }
    *out = result.text;
    return true;
}

} // namespace

struct Descrambler::Impl
{
    std::string js;
    std::unique_ptr<Script> script;
    std::map<std::string, Value> globals;
    JsFunction signature;
    std::vector<Value> signature_prefix;
    JsFunction throttle;
    std::vector<Value> throttle_prefix;
    bool has_signature = false;
    bool has_throttle = false;
    bool ready = false;
    std::string js_url;
};

Descrambler::Descrambler() : impl_(std::make_unique<Impl>())
{
}

Descrambler::~Descrambler() = default;

bool Descrambler::prepare(std::string_view player_js_url, std::string *error)
{
    impl_->ready = false;
    impl_->js_url.clear();
    if (player_js_url.empty())
    {
        if (error != nullptr)
            *error = "no player JS URL";
        return false;
    }

    http::Request request;
    request.url = std::string(player_js_url);
    request.headers = {{"User-Agent", kPlayerUserAgent}};
    request.timeout_s = 20;
    request.max_bytes = kMaxJsBytes;
    const http::Response response = http::perform(request);
    if (!response.error.empty())
    {
        if (error != nullptr)
            *error = response.error;
        return false;
    }
    if (!response.http_ok())
    {
        if (error != nullptr)
            *error = "http " + std::to_string(response.status);
        return false;
    }
    if (!prepare_js(response.body, error))
        return false;
    impl_->js_url = std::string(player_js_url);
    return true;
}

bool Descrambler::prepare_js(std::string_view player_js, std::string *error)
{
    impl_->ready = false;
    impl_->js_url.clear();
    impl_->has_signature = false;
    impl_->has_throttle = false;
    impl_->signature = JsFunction{};
    impl_->signature_prefix.clear();
    impl_->throttle = JsFunction{};
    impl_->throttle_prefix.clear();
    impl_->globals.clear();
    if (player_js.empty())
    {
        if (error != nullptr)
            *error = "empty player JS";
        return false;
    }
    if (player_js.size() > kMaxJsBytes)
    {
        if (error != nullptr)
            *error = "player JS too large";
        return false;
    }
    impl_->js.assign(player_js);
    impl_->script = std::make_unique<Script>(impl_->js);

    // The player's own tables (its string table and its operation tables), so
    // the interpreter can read the obfuscated helpers.
    std::string table_name;
    std::vector<std::string> words;
    if (parse_string_table(impl_->js, &table_name, &words))
    {
        Value value;
        value.kind = Value::Kind::words;
        value.words = std::make_shared<std::vector<std::string>>(std::move(words));
        impl_->globals[table_name] = std::move(value);
    }
    parse_object_tables(impl_->js, &impl_->globals);

    std::string snippet;

    // The plain shape first: the setter names the helper directly. A helper it
    // names but the subset cannot run is refused here, with the operation.
    JsFunction signature;
    if (extract_signature(impl_->js, impl_->script.get(), &signature, /*allow_fallback=*/false))
    {
        if (signature.body.size() > kMaxFunctionBytes)
        {
            if (error != nullptr)
                *error = "signature function too large";
            return false;
        }
        if (find_unsupported(signature.body, &snippet))
        {
            if (error != nullptr)
                *error = "unsupported player operation: " + snippet;
            return false;
        }
        impl_->signature = std::move(signature);
        impl_->has_signature = true;
    }

    JsFunction throttle;
    std::string throttle_note;
    if (extract_throttle(impl_->js, impl_->script.get(), &throttle, &throttle_note))
    {
        if (throttle.body.size() > kMaxFunctionBytes)
        {
            if (error != nullptr)
                *error = "throttle function too large";
            return false;
        }
        if (find_unsupported(throttle.body, &snippet))
        {
            if (error != nullptr)
                *error = "unsupported player operation in the throttle: " + snippet;
            return false;
        }
        impl_->throttle = std::move(throttle);
        impl_->has_throttle = true;
    }
    else if (!throttle_note.empty())
    {
        // Not fatal: the signature still plays. Said here so a silent
        // pass-through is not mistaken for a build with no throttle at all.
        yt::log::line("decipher: no throttle helper: %s", throttle_note.c_str());
    }

    // The obfuscated shape: a helper that drives the player's own tables. Its
    // selector arguments come from its call sites; the first that transforms a
    // sample is the one the player uses for the signature. It is not claimed
    // as the throttle: nothing ties it to the n call site.
    if (!impl_->has_signature)
    {
        std::string helper_name;
        JsFunction helper;
        if (find_table_helper(impl_->js, impl_->globals, &helper_name, &helper))
        {
            std::vector<Value> prefix;
            const std::string sample = "abcdefghij";
            for (const Selector &selector : find_selectors(impl_->js, helper_name))
            {
                std::vector<Value> candidate = {number_value(selector.first),
                                                number_value(selector.second)};
                std::string probe;
                std::string why;
                if (run_helper(impl_->script.get(), impl_->globals, helper, candidate, sample,
                               &probe, &why) &&
                    probe != sample)
                {
                    prefix = std::move(candidate);
                    break;
                }
            }
            impl_->signature = helper;
            impl_->signature_prefix = prefix;
            impl_->has_signature = true;
            yt::log::line("decipher: table helper %s, %s selector", helper_name.c_str(),
                          prefix.empty() ? "no" : "with");
        }
    }

    // Last resort: any function in the file that splits and joins.
    if (!impl_->has_signature)
    {
        JsFunction candidate;
        if (extract_signature(impl_->js, impl_->script.get(), &candidate,
                              /*allow_fallback=*/true) &&
            candidate.body.size() <= kMaxFunctionBytes &&
            !find_unsupported(candidate.body, &snippet))
        {
            impl_->signature = std::move(candidate);
            impl_->has_signature = true;
        }
    }

    if (!impl_->has_signature && !impl_->has_throttle)
    {
        if (error != nullptr)
            *error = "no signature or throttle helper found";
        return false;
    }
    yt::log::line("decipher: helpers signature=%s throttle=%s", impl_->has_signature ? "yes" : "no",
                  impl_->has_throttle ? "yes" : "no");
    impl_->ready = true;
    return true;
}

std::string Descrambler::descramble(std::string_view url, std::string_view s, std::string_view sp,
                                    std::string *error) const
{
    if (!impl_->ready || impl_->script == nullptr)
    {
        if (error != nullptr)
            *error = "descrambler not prepared";
        return {};
    }

    std::string result(url);
    if (!s.empty() && impl_->has_signature)
    {
        std::string signature;
        std::string why;
        if (!run_helper(impl_->script.get(), impl_->globals, impl_->signature,
                        impl_->signature_prefix, std::string(s), &signature, &why))
        {
            if (error != nullptr)
                *error = "signature: " + why;
            return {};
        }
        if (!sp.empty())
        {
            result += (result.find('?') == std::string::npos ? '?' : '&');
            result += std::string(sp);
            result += '=';
            result += encode_uri_component(signature);
        }
    }

    if (impl_->has_throttle)
    {
        // A query "n" first (the classic shape), then the TV shape
        // "/n/<value>/videoplayback"; the player's own DOr reads the query
        // value and rewrites the path segment with it, so both are updated.
        std::string throttle;
        bool have = param_value(result, "n", &throttle) && !throttle.empty();
        const bool has_query = have;
        bool has_path = false;
        std::size_t path_begin = 0;
        std::size_t path_end = 0;
        {
            const std::size_t query = result.find('?');
            const std::string_view head = query == std::string::npos
                                              ? std::string_view(result)
                                              : std::string_view(result).substr(0, query);
            const std::size_t marker = head.find("/n/");
            if (marker != std::string_view::npos)
            {
                path_begin = marker + 3;
                path_end = head.find('/', path_begin);
                if (path_end == std::string_view::npos)
                    path_end = head.size();
                if (path_end > path_begin)
                {
                    has_path = true;
                    if (!have)
                    {
                        throttle = std::string(head.substr(path_begin, path_end - path_begin));
                        have = true;
                    }
                }
            }
        }
        if (have)
        {
            std::string throttled;
            std::string why;
            if (!run_helper(impl_->script.get(), impl_->globals, impl_->throttle,
                            impl_->throttle_prefix, percent_decode(throttle), &throttled, &why))
            {
                if (error != nullptr)
                    *error = "throttle: " + why;
                return {};
            }
            if (has_path)
                result.replace(path_begin, path_end - path_begin, throttled);
            if (has_query)
                set_param(&result, "n", encode_uri_component(throttled));
        }
    }

    return result;
}

bool Descrambler::ready() const
{
    return impl_->ready;
}

} // namespace yt
