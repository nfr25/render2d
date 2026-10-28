// SPDX-License-Identifier: AGPL-3.0-or-later
#include "style/filter.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace r2d {

// --- 1. The lexer: text -> words. notions §29 -------------------------------

namespace {

enum class Tok { Ident, Number, Text, Op, LParen, RParen, Comma, Keyword, End };

struct Token {
    Tok         kind = Tok::End;
    std::string text;       // identifier, text literal, operator or keyword (upper case)
    double      number = 0.0;
    std::size_t pos    = 0; // 0-based position in the filter text
};

struct ParseError : std::runtime_error {
    std::size_t pos;
    ParseError(std::size_t p, const std::string& m) : std::runtime_error(m), pos(p) {}
};

const char* const kKeywords[] = {"AND", "OR", "NOT", "IS", "NULL", "BETWEEN", "IN", "LIKE"};

std::vector<Token> lex(const std::string& s) {
    std::vector<Token> out;
    std::size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        Token t;
        t.pos = i;
        if (c == '\'') {                                 // 'text', with '' for an apostrophe
            t.kind = Tok::Text;
            std::size_t j = i + 1;
            for (;;) {
                if (j >= s.size()) throw ParseError(i, "text not closed (missing ')");
                if (s[j] == '\'') {
                    if (j + 1 < s.size() && s[j + 1] == '\'') { t.text += '\''; j += 2; continue; }
                    break;
                }
                t.text += s[j++];
            }
            i = j + 1;
        } else if (c == '"') {                           // "field name" (CQL quoted identifier)
            const std::size_t j = s.find('"', i + 1);
            if (j == std::string::npos) throw ParseError(i, "field name not closed (missing \")");
            t.kind = Tok::Ident;
            t.text = s.substr(i + 1, j - i - 1);
            i = j + 1;
        } else if (std::isdigit(static_cast<unsigned char>(c)) || c == '.' ||
                   (c == '-' && i + 1 < s.size() && (std::isdigit(static_cast<unsigned char>(s[i + 1])) || s[i + 1] == '.'))) {
            char* end = nullptr;
            t.kind   = Tok::Number;
            t.number = std::strtod(s.c_str() + i, &end);
            if (end == s.c_str() + i) throw ParseError(i, "number expected");
            i = static_cast<std::size_t>(end - s.c_str());
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            std::size_t j = i;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '_')) ++j;
            t.text = s.substr(i, j - i);
            std::string up = t.text;
            for (char& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            t.kind = Tok::Ident;
            for (const char* k : kKeywords) if (up == k) { t.kind = Tok::Keyword; t.text = up; }
            i = j;
        } else if (c == '(' || c == ')' || c == ',') {
            t.kind = c == '(' ? Tok::LParen : c == ')' ? Tok::RParen : Tok::Comma;
            t.text = std::string(1, c);
            ++i;
        } else if (c == '<' || c == '>' || c == '=' || c == '!') {
            const std::string two = s.substr(i, 2);
            t.kind = Tok::Op;
            if (two == "<=" || two == ">=" || two == "<>" || two == "!=") { t.text = two; i += 2; }
            else if (c != '!') { t.text = std::string(1, c); ++i; }
            else throw ParseError(i, "unexpected character '!'");
        } else {
            throw ParseError(i, std::string("unexpected character '") + c + "'");
        }
        out.push_back(std::move(t));
    }
    Token end;
    end.pos = s.size();
    out.push_back(end);
    return out;
}

std::string describe(const Token& t) {
    switch (t.kind) {
        case Tok::End:  return "the end of the filter";
        case Tok::Text: return "'" + t.text + "'";
        default:        return "'" + t.text + "'";
    }
}

} // namespace

// --- 2. The parser: words -> tree. Priority: comparison, NOT, AND, OR -----

class FilterParser {
public:
    FilterParser(Filter& f, std::vector<Token> toks) : f_(f), t_(std::move(toks)) {}

    int parseAll() {
        const int root = parseOr();
        if (peek().kind != Tok::End) throw ParseError(peek().pos, "unexpected " + describe(peek()));
        return root;
    }

private:
    using Kind = Filter::Kind;
    Filter&            f_;
    std::vector<Token> t_;
    std::size_t        p_ = 0;

    const Token& peek() const { return t_[p_]; }
    const Token& take() { return t_[p_ < t_.size() - 1 ? p_++ : p_]; }
    bool isKeyword(const char* k) const { return peek().kind == Tok::Keyword && peek().text == k; }
    void expectKeyword(const char* k) {
        if (!isKeyword(k)) throw ParseError(peek().pos, std::string(k) + " expected, found " + describe(peek()));
        take();
    }

    int add(Filter::Node n) {
        f_.nodes_.push_back(std::move(n));
        return static_cast<int>(f_.nodes_.size() - 1);
    }

    // A field name or a literal.
    int parseValue() {
        const Token& t = take();
        Filter::Node n;
        switch (t.kind) {
            case Tok::Ident:  n.kind = Kind::Field;  n.text = t.text;     break;
            case Tok::Number: n.kind = Kind::Number; n.number = t.number; break;
            case Tok::Text:   n.kind = Kind::Text;   n.text = t.text;     break;
            default: throw ParseError(t.pos, "a field name or a value expected, found " + describe(t));
        }
        return add(std::move(n));
    }

    int parsePredicate() {
        if (peek().kind == Tok::LParen) {
            take();
            const int inner = parseOr();
            if (peek().kind != Tok::RParen) throw ParseError(peek().pos, "')' expected, found " + describe(peek()));
            take();
            return inner;
        }
        const int value = parseValue();
        Filter::Node n;
        n.kids.push_back(value);

        if (isKeyword("IS")) {                                      // IS [NOT] NULL
            take();
            if (isKeyword("NOT")) { take(); n.negate = true; }
            expectKeyword("NULL");
            n.kind = Kind::IsNull;
            return add(std::move(n));
        }
        if (isKeyword("NOT")) { take(); n.negate = true; }          // NOT BETWEEN / IN / LIKE
        if (isKeyword("BETWEEN")) {
            take();
            n.kids.push_back(parseValue());
            expectKeyword("AND");
            n.kids.push_back(parseValue());
            n.kind = Kind::Between;
            return add(std::move(n));
        }
        if (isKeyword("IN")) {
            take();
            if (peek().kind != Tok::LParen) throw ParseError(peek().pos, "'(' expected after IN");
            take();
            n.kids.push_back(parseValue());
            while (peek().kind == Tok::Comma) { take(); n.kids.push_back(parseValue()); }
            if (peek().kind != Tok::RParen) throw ParseError(peek().pos, "')' or ',' expected, found " + describe(peek()));
            take();
            n.kind = Kind::In;
            return add(std::move(n));
        }
        if (isKeyword("LIKE")) {
            take();
            const Token& pat = take();
            if (pat.kind != Tok::Text) throw ParseError(pat.pos, "LIKE expects a text pattern, like 'Avenue%'");
            n.kind = Kind::Like;
            n.text = pat.text;
            return add(std::move(n));
        }
        if (n.negate) throw ParseError(peek().pos, "BETWEEN, IN or LIKE expected after NOT");

        const Token& op = take();
        if (op.kind != Tok::Op) throw ParseError(op.pos, "a comparison (=, <>, <, <=, >, >=) expected, found " + describe(op));
        using Op = Filter::Op;
        n.op = op.text == "="  ? Op::Eq : (op.text == "<>" || op.text == "!=") ? Op::Ne
             : op.text == "<"  ? Op::Lt : op.text == "<=" ? Op::Le
             : op.text == ">"  ? Op::Gt : Op::Ge;
        n.kids.push_back(parseValue());
        n.kind = Kind::Compare;
        return add(std::move(n));
    }

    int parseNot() {
        if (isKeyword("NOT")) {
            take();
            Filter::Node n;
            n.kind = Kind::Not;
            n.kids.push_back(parseNot());
            return add(std::move(n));
        }
        return parsePredicate();
    }

    int parseBinary(Kind kind, const char* word, int (FilterParser::*next)()) {
        int left = (this->*next)();
        while (isKeyword(word)) {
            take();
            Filter::Node n;
            n.kind = kind;
            n.kids = {left, (this->*next)()};
            left = add(std::move(n));
        }
        return left;
    }
    int parseAnd() { return parseBinary(Kind::And, "AND", &FilterParser::parseNot); }
    int parseOr()  { return parseBinary(Kind::Or,  "OR",  &FilterParser::parseAnd); }
};

std::optional<Filter> Filter::parse(const std::string& text, std::string& error) {
    Filter f;
    f.text_ = text;
    try {
        FilterParser p(f, lex(text));
        f.root_ = p.parseAll();
    } catch (const ParseError& e) {
        error = "column " + std::to_string(e.pos + 1) + ": " + e.what();
        return std::nullopt;
    }
    return f;
}

// --- 3. Binding: names -> columns, once per pass ----------------------------

std::vector<std::string> Filter::fieldNames() const {
    std::vector<std::string> out;
    for (const Node& n : nodes_) {
        if (n.kind != Kind::Field) continue;
        bool seen = false;
        for (const auto& s : out) seen = seen || s == n.text;
        if (!seen) out.push_back(n.text);
    }
    return out;
}

std::vector<std::string> Filter::bind(const std::vector<FieldDef>& fields) {
    std::vector<std::string> missing;
    for (Node& n : nodes_) {
        if (n.kind != Kind::Field) continue;
        n.column = -1;
        for (std::size_t i = 0; i < fields.size(); ++i)
            if (fields[i].name == n.text) { n.column = static_cast<int>(i); break; }
        if (n.column < 0) {
            bool seen = false;
            for (const auto& s : missing) seen = seen || s == n.text;
            if (!seen) missing.push_back(n.text);
        }
    }
    return missing;
}

std::vector<int> Filter::columns() const {
    std::vector<int> out;
    for (const Node& n : nodes_)
        if (n.kind == Kind::Field && n.column >= 0) out.push_back(n.column);
    return out;
}

// --- 4. Evaluation, three-valued. notions §28 -------------------------------

namespace {

// '106' -> 106; '75B' -> not a number. Surrounding spaces are allowed.
bool parseNumber(const std::string& s, double& out) {
    const char* b = s.c_str();
    char* end = nullptr;
    out = std::strtod(b, &end);
    if (end == b) return false;
    while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
    return *end == '\0';
}

std::string numberText(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.15g", v);
    return buf;
}

// LIKE: % = any run of characters, _ = exactly one. Case-sensitive, as in CQL.
bool like(const char* s, const char* p) {
    const char* star = nullptr;
    const char* back = nullptr;
    while (*s) {
        if (*p == '%') { star = p++; back = s; continue; }
        if (*p == '_' || *p == *s) { ++s; ++p; continue; }
        if (star) { p = star + 1; s = ++back; continue; }
        return false;
    }
    while (*p == '%') ++p;
    return *p == '\0';
}

Tri fromBool(bool b) { return b ? Tri::True : Tri::False; }

} // namespace

class FilterEval {
public:
    FilterEval(const Filter& f, const Attributes& row, FilterStats& st) : f_(f), row_(row), st_(st) {}

    Tri eval(int i) {
        using Kind = Filter::Kind;
        const Filter::Node& n = f_.nodes_[i];
        switch (n.kind) {
            case Kind::IsNull: {
                const bool null = isNullValue(n.kids[0]);
                return fromBool(n.negate ? !null : null);
            }
            case Kind::Not:  return negate(eval(n.kids[0]));
            case Kind::And: {
                const Tri a = eval(n.kids[0]);
                if (a == Tri::False) return Tri::False;
                const Tri b = eval(n.kids[1]);
                if (b == Tri::False) return Tri::False;
                return a == Tri::True && b == Tri::True ? Tri::True : Tri::Unknown;
            }
            case Kind::Or: {
                const Tri a = eval(n.kids[0]);
                if (a == Tri::True) return Tri::True;
                const Tri b = eval(n.kids[1]);
                if (b == Tri::True) return Tri::True;
                return a == Tri::False && b == Tri::False ? Tri::False : Tri::Unknown;
            }
            case Kind::Compare: return compare(n.op, n.kids[0], n.kids[1]);
            case Kind::Between: {
                const Tri ge = compare(Filter::Op::Ge, n.kids[0], n.kids[1]);
                const Tri le = ge == Tri::False ? Tri::False : compare(Filter::Op::Le, n.kids[0], n.kids[2]);
                const Tri r = (ge == Tri::False || le == Tri::False) ? Tri::False
                            : (ge == Tri::True && le == Tri::True) ? Tri::True : Tri::Unknown;
                return n.negate ? negate(r) : r;
            }
            case Kind::In: {
                Tri r = Tri::False;
                for (std::size_t k = 1; k < n.kids.size() && r != Tri::True; ++k) {
                    const Tri e = compare(Filter::Op::Eq, n.kids[0], n.kids[k]);
                    if (e == Tri::True) r = Tri::True;
                    else if (e == Tri::Unknown) r = Tri::Unknown;
                }
                return n.negate ? negate(r) : r;
            }
            case Kind::Like: {
                std::string s;
                if (!text(n.kids[0], s)) return Tri::Unknown;
                const bool m = like(s.c_str(), n.text.c_str());
                return fromBool(n.negate ? !m : m);
            }
            default:   // a bare value is not a condition (the parser never builds this)
                return Tri::Unknown;
        }
    }

private:
    const Filter&     f_;
    const Attributes& row_;
    FilterStats&      st_;

    static Tri negate(Tri t) { return t == Tri::Unknown ? Tri::Unknown : fromBool(t == Tri::False); }

    const Value* fieldValue(const Filter::Node& n) const {
        if (n.column < 0 || n.column >= static_cast<int>(row_.size())) return nullptr;   // unknown -> NULL
        return &row_[n.column];
    }

    bool isNullValue(int i) const {
        const Filter::Node& n = f_.nodes_[i];
        if (n.kind != Filter::Kind::Field) return false;      // a literal is never NULL
        const Value* v = fieldValue(n);
        return !v || isNull(*v);
    }

    bool isNumeric(int i) const {
        const Filter::Node& n = f_.nodes_[i];
        if (n.kind == Filter::Kind::Number) return true;
        if (n.kind != Filter::Kind::Field) return false;
        const Value* v = fieldValue(n);
        return v && (std::holds_alternative<long long>(*v) || std::holds_alternative<double>(*v));
    }

    // False = NULL or not a number (counted).
    bool number(int i, double& out) {
        const Filter::Node& n = f_.nodes_[i];
        if (n.kind == Filter::Kind::Number) { out = n.number; return true; }
        if (n.kind == Filter::Kind::Text) {
            if (parseNumber(n.text, out)) return true;
            ++st_.notNumber;
            return false;
        }
        const Value* v = fieldValue(n);
        if (!v || isNull(*v)) return false;
        if (const auto* x = std::get_if<long long>(v)) { out = static_cast<double>(*x); return true; }
        if (const auto* x = std::get_if<double>(v))    { out = *x; return true; }
        if (parseNumber(std::get<std::string>(*v), out)) return true;
        ++st_.notNumber;
        return false;
    }

    // False = NULL.
    bool text(int i, std::string& out) const {
        const Filter::Node& n = f_.nodes_[i];
        if (n.kind == Filter::Kind::Text)   { out = n.text; return true; }
        if (n.kind == Filter::Kind::Number) { out = numberText(n.number); return true; }
        const Value* v = fieldValue(n);
        if (!v || isNull(*v)) return false;
        if (const auto* s = std::get_if<std::string>(v)) { out = *s; return true; }
        if (const auto* x = std::get_if<long long>(v))   { out = std::to_string(*x); return true; }
        out = numberText(std::get<double>(*v));
        return true;
    }

    // notions §28: the literal decides. A number literal -> compare as numbers;
    // a text literal -> as texts; two fields -> as numbers if both are numbers.
    Tri compare(Filter::Op op, int a, int b) {
        const Filter::Kind ka = f_.nodes_[a].kind, kb = f_.nodes_[b].kind;
        bool numeric;
        if (ka == Filter::Kind::Number || kb == Filter::Kind::Number)  numeric = true;
        else if (ka == Filter::Kind::Text || kb == Filter::Kind::Text) numeric = false;
        else {
            if (isNullValue(a) || isNullValue(b)) return Tri::Unknown;
            numeric = isNumeric(a) && isNumeric(b);
        }
        int c;   // -1, 0, +1
        if (numeric) {
            double x, y;
            if (isNullValue(a) || isNullValue(b)) return Tri::Unknown;
            if (!number(a, x) || !number(b, y)) return Tri::Unknown;
            c = x < y ? -1 : x > y ? 1 : 0;
        } else {
            std::string x, y;
            if (!text(a, x) || !text(b, y)) return Tri::Unknown;
            c = x.compare(y);
            c = c < 0 ? -1 : c > 0 ? 1 : 0;
        }
        switch (op) {
            case Filter::Op::Eq: return fromBool(c == 0);
            case Filter::Op::Ne: return fromBool(c != 0);
            case Filter::Op::Lt: return fromBool(c < 0);
            case Filter::Op::Le: return fromBool(c <= 0);
            case Filter::Op::Gt: return fromBool(c > 0);
            case Filter::Op::Ge: return fromBool(c >= 0);
        }
        return Tri::Unknown;
    }
};

Tri Filter::eval(const Attributes& row, FilterStats& stats) const {
    if (root_ < 0) return Tri::Unknown;
    return FilterEval(*this, row, stats).eval(root_);
}

// --- 5. Dump ------------------------------------------------------------------

namespace {

const char* opText(Filter::Op op) {
    switch (op) {
        case Filter::Op::Eq: return "=";  case Filter::Op::Ne: return "<>";
        case Filter::Op::Lt: return "<";  case Filter::Op::Le: return "<=";
        case Filter::Op::Gt: return ">";  case Filter::Op::Ge: return ">=";
    }
    return "?";
}

std::string dumpNode(const std::vector<Filter::Node>& nodes, int i) {
    using Kind = Filter::Kind;
    const Filter::Node& n = nodes[i];
    auto kid = [&](std::size_t k) { return dumpNode(nodes, n.kids[k]); };
    const std::string no = n.negate ? "NOT " : "";
    switch (n.kind) {
        case Kind::Field:   return n.text;
        case Kind::Number:  return numberText(n.number);
        case Kind::Text:    return "'" + n.text + "'";
        case Kind::Compare: return std::string("(") + opText(n.op) + " " + kid(0) + " " + kid(1) + ")";
        case Kind::Between: return "(" + no + "BETWEEN " + kid(0) + " " + kid(1) + " " + kid(2) + ")";
        case Kind::In: {
            std::string s = "(" + no + "IN " + kid(0);
            for (std::size_t k = 1; k < n.kids.size(); ++k) s += " " + kid(k);
            return s + ")";
        }
        case Kind::Like:    return "(" + no + "LIKE " + kid(0) + " '" + n.text + "')";
        case Kind::IsNull:  return std::string("(") + (n.negate ? "IS NOT NULL " : "IS NULL ") + kid(0) + ")";
        case Kind::Not:     return "(NOT " + kid(0) + ")";
        case Kind::And:     return "(AND " + kid(0) + " " + kid(1) + ")";
        case Kind::Or:      return "(OR " + kid(0) + " " + kid(1) + ")";
    }
    return "?";
}

} // namespace

std::string Filter::dump() const { return root_ < 0 ? "" : dumpNode(nodes_, root_); }

} // namespace r2d
