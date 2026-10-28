// SPDX-License-Identifier: AGPL-3.0-or-later
#include "style/expression.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace r2d {

using Kind = Expression::Kind;
using Node = Expression::Node;

// --- 1. The lexer: text -> words. notions §29 -------------------------------

namespace {

enum class Tok { Ident, Number, Text, Op, Arith, LParen, RParen, Comma, Keyword, End };

struct Token {
    Tok         kind = Tok::End;
    std::string text;       // identifier, text literal, operator or keyword (upper case)
    double      number = 0.0;
    std::size_t pos    = 0; // 0-based position in the text
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
        } else if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
            // No sign here: "360-3" is 360 minus 3. A leading minus is unary (parser).
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
        } else if (c == '+' || c == '-' || c == '*' || c == '/') {
            t.kind = Tok::Arith;
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
    return t.kind == Tok::End ? "the end of the expression" : "'" + t.text + "'";
}

bool isCondition(Kind k) {
    switch (k) {
        case Kind::Compare: case Kind::Between: case Kind::In: case Kind::Like:
        case Kind::IsNull:  case Kind::Not:     case Kind::And: case Kind::Or: return true;
        default: return false;
    }
}

} // namespace

// --- 2. The parser: words -> tree --------------------------------------------
// Priorities, from the loosest to the tightest (notions §29, §36):
//   OR  <  AND  <  NOT  <  comparison  <  + -  <  * /  <  unary -  <  ( ) and values

class ExprParser {
public:
    ExprParser(Expression& e, std::vector<Token> toks) : e_(e), t_(std::move(toks)) {}

    int parseAll() {
        const int root = parseOr();
        if (peek().kind != Tok::End) throw ParseError(peek().pos, "unexpected " + describe(peek()));
        return root;
    }

    // Conditions where conditions belong, values where values belong.
    void check(int i) const {
        const Node& n = e_.nodes_[i];
        switch (n.kind) {
            case Kind::Not: case Kind::And: case Kind::Or:
                for (int k : n.kids) if (!isCondition(e_.nodes_[k].kind))
                    throw ParseError(e_.nodes_[k].pos, "a condition expected (a comparison, IS NULL, ...)");
                break;
            case Kind::Field: case Kind::Number: case Kind::Text: break;
            default:   // Arith, Neg and predicates take values
                for (int k : n.kids) if (isCondition(e_.nodes_[k].kind))
                    throw ParseError(e_.nodes_[k].pos, "a value expected here, not a condition");
        }
        for (int k : n.kids) check(k);
    }

private:
    Expression&        e_;
    std::vector<Token> t_;
    std::size_t        p_ = 0;

    const Token& peek() const { return t_[p_]; }
    const Token& take() { return t_[p_ < t_.size() - 1 ? p_++ : p_]; }
    bool isKeyword(const char* k) const { return peek().kind == Tok::Keyword && peek().text == k; }
    bool isArith(const char* ops) const {
        return peek().kind == Tok::Arith && std::string(ops).find(peek().text[0]) != std::string::npos;
    }
    void expectKeyword(const char* k) {
        if (!isKeyword(k)) throw ParseError(peek().pos, std::string(k) + " expected, found " + describe(peek()));
        take();
    }

    int add(Node n) {
        e_.nodes_.push_back(std::move(n));
        return static_cast<int>(e_.nodes_.size() - 1);
    }

    // ( ... ), a field name or a literal.
    int parsePrimary() {
        const Token& t = take();
        Node n;
        n.pos = t.pos;
        switch (t.kind) {
            case Tok::LParen: {
                const int inner = parseOr();
                if (peek().kind != Tok::RParen) throw ParseError(peek().pos, "')' expected, found " + describe(peek()));
                take();
                return inner;
            }
            case Tok::Ident:
                if (peek().kind == Tok::LParen) return parseCall(t);       // sqrt(N)
                n.kind = Kind::Field;  n.text = t.text;
                break;
            case Tok::Number: n.kind = Kind::Number; n.number = t.number; break;
            case Tok::Text:   n.kind = Kind::Text;   n.text = t.text;     break;
            default: throw ParseError(t.pos, "a field name or a value expected, found " + describe(t));
        }
        return add(std::move(n));
    }

    // §41: a function, so far sqrt(x) and abs(x).
    int parseCall(const Token& name) {
        std::string up = name.text;
        for (char& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (up != "SQRT" && up != "ABS") throw ParseError(name.pos, "unknown function '" + name.text + "' (known: sqrt, abs)");
        take();                                                          // (
        Node n;
        n.kind = Kind::Func;
        n.text = up;
        n.pos  = name.pos;
        n.kids.push_back(parseAdd());
        if (peek().kind != Tok::RParen) throw ParseError(peek().pos, "')' expected, found " + describe(peek()));
        take();
        return add(std::move(n));
    }

    int parseUnary() {
        if (isArith("-")) {
            const std::size_t pos = take().pos;
            const int v = parseUnary();
            if (e_.nodes_[v].kind == Kind::Number) {           // -2.5 is a number, not an operation
                e_.nodes_[v].number = -e_.nodes_[v].number;
                e_.nodes_[v].pos = pos;
                return v;
            }
            Node n;
            n.kind = Kind::Neg;
            n.pos  = pos;
            n.kids = {v};
            return add(std::move(n));
        }
        return parsePrimary();
    }

    int parseArith(const char* ops, int (ExprParser::*next)()) {
        int left = (this->*next)();
        while (isArith(ops)) {
            const Token& op = take();
            Node n;
            n.kind  = Kind::Arith;
            n.arith = op.text[0];
            n.pos   = op.pos;
            n.kids  = {left, (this->*next)()};
            left = add(std::move(n));
        }
        return left;
    }
    int parseMul() { return parseArith("*/", &ExprParser::parseUnary); }
    int parseAdd() { return parseArith("+-", &ExprParser::parseMul); }

    int parseComparison() {
        const int value = parseAdd();
        Node n;
        n.pos = e_.nodes_[value].pos;
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
            n.kids.push_back(parseAdd());
            expectKeyword("AND");
            n.kids.push_back(parseAdd());
            n.kind = Kind::Between;
            return add(std::move(n));
        }
        if (isKeyword("IN")) {
            take();
            if (peek().kind != Tok::LParen) throw ParseError(peek().pos, "'(' expected after IN");
            take();
            n.kids.push_back(parseAdd());
            while (peek().kind == Tok::Comma) { take(); n.kids.push_back(parseAdd()); }
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
        if (peek().kind != Tok::Op) return value;                   // a value alone, or ( condition )

        const Token& op = take();
        using Op = Expression::Op;
        n.op = op.text == "="  ? Op::Eq : (op.text == "<>" || op.text == "!=") ? Op::Ne
             : op.text == "<"  ? Op::Lt : op.text == "<=" ? Op::Le
             : op.text == ">"  ? Op::Gt : Op::Ge;
        n.kids.push_back(parseAdd());
        n.kind = Kind::Compare;
        return add(std::move(n));
    }

    int parseNot() {
        if (isKeyword("NOT")) {
            Node n;
            n.pos  = take().pos;
            n.kind = Kind::Not;
            n.kids.push_back(parseNot());
            return add(std::move(n));
        }
        return parseComparison();
    }

    int parseBinary(Kind kind, const char* word, int (ExprParser::*next)()) {
        int left = (this->*next)();
        while (isKeyword(word)) {
            Node n;
            n.pos  = take().pos;
            n.kind = kind;
            n.kids = {left, (this->*next)()};
            left = add(std::move(n));
        }
        return left;
    }
    int parseAnd() { return parseBinary(Kind::And, "AND", &ExprParser::parseNot); }
    int parseOr()  { return parseBinary(Kind::Or,  "OR",  &ExprParser::parseAnd); }
};

std::optional<Expression> Expression::parseCondition(const std::string& text, std::string& error) {
    Expression e;
    e.text_ = text;
    try {
        ExprParser p(e, lex(text));
        e.root_ = p.parseAll();
        p.check(e.root_);
        if (!isCondition(e.nodes_[e.root_].kind))
            throw ParseError(0, "a condition expected (a comparison such as AREA < 30), not just a value");
    } catch (const ParseError& x) {
        error = "column " + std::to_string(x.pos + 1) + ": " + x.what();
        return std::nullopt;
    }
    return e;
}

std::optional<Expression> Expression::parseValue(const std::string& text, std::string& error) {
    Expression e;
    e.text_ = text;
    try {
        ExprParser p(e, lex(text));
        e.root_ = p.parseAll();
        p.check(e.root_);
    } catch (const ParseError& x) {
        error = "column " + std::to_string(x.pos + 1) + ": " + x.what();
        return std::nullopt;
    }
    return e;
}

bool Expression::isLiteralText() const {
    return nodes_.size() == 1 && nodes_[0].kind == Kind::Text && nodes_[0].text == text_;
}

Expression Expression::literalText(const std::string& text) {
    Expression e;
    e.text_ = text;
    Node n;
    n.kind = Kind::Text;
    n.text = text;
    e.nodes_.push_back(n);
    e.root_ = 0;
    return e;
}

// --- 3. Binding: names -> columns, once per pass ----------------------------

std::vector<std::string> Expression::fieldNames() const {
    std::vector<std::string> out;
    for (const Node& n : nodes_) {
        if (n.kind != Kind::Field) continue;
        bool seen = false;
        for (const auto& s : out) seen = seen || s == n.text;
        if (!seen) out.push_back(n.text);
    }
    return out;
}

std::vector<std::string> Expression::bind(const std::vector<FieldDef>& fields) {
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

std::vector<int> Expression::columns() const {
    std::vector<int> out;
    for (const Node& n : nodes_)
        if (n.kind == Kind::Field && n.column >= 0) out.push_back(n.column);
    return out;
}

// --- 4. Evaluation. notions §28 and §36 ---------------------------------------

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

class ExprEval {
public:
    ExprEval(const Expression& e, const Attributes& row, EvalStats& st) : e_(e), row_(row), st_(st) {}

    Tri test(int i) {
        const Node& n = e_.nodes_[i];
        switch (n.kind) {
            case Kind::IsNull: {
                const bool null = isNull(value(n.kids[0]));
                return fromBool(n.negate ? !null : null);
            }
            case Kind::Not:  return negate(test(n.kids[0]));
            case Kind::And: {
                const Tri a = test(n.kids[0]);
                if (a == Tri::False) return Tri::False;
                const Tri b = test(n.kids[1]);
                if (b == Tri::False) return Tri::False;
                return a == Tri::True && b == Tri::True ? Tri::True : Tri::Unknown;
            }
            case Kind::Or: {
                const Tri a = test(n.kids[0]);
                if (a == Tri::True) return Tri::True;
                const Tri b = test(n.kids[1]);
                if (b == Tri::True) return Tri::True;
                return a == Tri::False && b == Tri::False ? Tri::False : Tri::Unknown;
            }
            case Kind::Compare: return compare(n.op, n.kids[0], n.kids[1]);
            case Kind::Between: {
                const Tri ge = compare(Expression::Op::Ge, n.kids[0], n.kids[1]);
                const Tri le = ge == Tri::False ? Tri::False : compare(Expression::Op::Le, n.kids[0], n.kids[2]);
                const Tri r = (ge == Tri::False || le == Tri::False) ? Tri::False
                            : (ge == Tri::True && le == Tri::True) ? Tri::True : Tri::Unknown;
                return n.negate ? negate(r) : r;
            }
            case Kind::In: {
                Tri r = Tri::False;
                for (std::size_t k = 1; k < n.kids.size() && r != Tri::True; ++k) {
                    const Tri e = compare(Expression::Op::Eq, n.kids[0], n.kids[k]);
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
            default: {   // a value used as a condition: non-zero number = true
                double x;
                if (!number(i, x)) return Tri::Unknown;
                return fromBool(x != 0.0);
            }
        }
    }

    Value value(int i) {
        const Node& n = e_.nodes_[i];
        switch (n.kind) {
            case Kind::Number: return n.number;
            case Kind::Text:   return n.text;
            case Kind::Field: {
                const Value* v = fieldValue(n);
                return v ? *v : Value{};
            }
            case Kind::Arith: case Kind::Neg: case Kind::Func: {
                double x;
                if (!number(i, x)) return Value{};
                return x;
            }
            default: {                                   // a condition: 1, 0 or NULL
                const Tri t = test(i);
                if (t == Tri::Unknown) return Value{};
                return t == Tri::True ? 1.0 : 0.0;
            }
        }
    }

private:
    const Expression& e_;
    const Attributes& row_;
    EvalStats&        st_;

    static Tri negate(Tri t) { return t == Tri::Unknown ? Tri::Unknown : fromBool(t == Tri::False); }

    const Value* fieldValue(const Node& n) const {
        if (n.column < 0 || n.column >= static_cast<int>(row_.size())) return nullptr;   // unknown -> NULL
        return &row_[n.column];
    }

    // The node as a number. False = NULL, not a number (counted) or division by zero.
    bool number(int i, double& out) {
        const Node& n = e_.nodes_[i];
        switch (n.kind) {
            case Kind::Number: out = n.number; return true;
            case Kind::Neg: {
                if (!number(n.kids[0], out)) return false;
                out = -out;
                return true;
            }
            case Kind::Func: {
                if (!number(n.kids[0], out)) return false;
                if (n.text == "ABS") { out = std::fabs(out); return true; }
                if (out < 0.0) return false;                 // sqrt of a negative: NULL
                out = std::sqrt(out);
                return true;
            }
            case Kind::Arith: {
                double x, y;
                if (!number(n.kids[0], x) || !number(n.kids[1], y)) return false;
                switch (n.arith) {
                    case '+': out = x + y; break;
                    case '-': out = x - y; break;
                    case '*': out = x * y; break;
                    default:
                        if (y == 0.0) return false;
                        out = x / y;
                }
                return true;
            }
            default: break;
        }
        const Value v = value(i);
        if (isNull(v)) return false;
        if (const auto* x = std::get_if<long long>(&v)) { out = static_cast<double>(*x); return true; }
        if (const auto* x = std::get_if<double>(&v))    { out = *x; return true; }
        if (parseNumber(std::get<std::string>(v), out)) return true;
        ++st_.notNumber;
        return false;
    }

    // The node as a text. False = NULL.
    bool text(int i, std::string& out) {
        const Value v = value(i);
        if (isNull(v)) return false;
        if (const auto* s = std::get_if<std::string>(&v)) { out = *s; return true; }
        if (const auto* x = std::get_if<long long>(&v))   { out = std::to_string(*x); return true; }
        out = numberText(std::get<double>(v));
        return true;
    }

    bool isNumericValue(int i) {
        const Value v = value(i);
        return std::holds_alternative<long long>(v) || std::holds_alternative<double>(v);
    }

    // notions §28: the literal decides. A number literal or arithmetic ->
    // compare as numbers; a text literal -> as texts; two fields -> as
    // numbers if both are numbers.
    Tri compare(Expression::Op op, int a, int b) {
        auto numericKind = [](Kind k) { return k == Kind::Number || k == Kind::Arith || k == Kind::Neg || k == Kind::Func; };
        const Kind ka = e_.nodes_[a].kind, kb = e_.nodes_[b].kind;
        bool numeric;
        if (numericKind(ka) || numericKind(kb))       numeric = true;
        else if (ka == Kind::Text || kb == Kind::Text) numeric = false;
        else {
            if (isNull(value(a)) || isNull(value(b))) return Tri::Unknown;
            numeric = isNumericValue(a) && isNumericValue(b);
        }
        int c;   // -1, 0, +1
        if (numeric) {
            double x, y;
            if (isNull(value(a)) || isNull(value(b))) return Tri::Unknown;
            if (!number(a, x) || !number(b, y)) return Tri::Unknown;
            c = x < y ? -1 : x > y ? 1 : 0;
        } else {
            std::string x, y;
            if (!text(a, x) || !text(b, y)) return Tri::Unknown;
            c = x.compare(y);
            c = c < 0 ? -1 : c > 0 ? 1 : 0;
        }
        switch (op) {
            case Expression::Op::Eq: return fromBool(c == 0);
            case Expression::Op::Ne: return fromBool(c != 0);
            case Expression::Op::Lt: return fromBool(c < 0);
            case Expression::Op::Le: return fromBool(c <= 0);
            case Expression::Op::Gt: return fromBool(c > 0);
            case Expression::Op::Ge: return fromBool(c >= 0);
        }
        return Tri::Unknown;
    }
};

Tri Expression::test(const Attributes& row, EvalStats& stats) const {
    if (root_ < 0) return Tri::Unknown;
    return ExprEval(*this, row, stats).test(root_);
}

Value Expression::value(const Attributes& row, EvalStats& stats) const {
    if (root_ < 0) return Value{};
    return ExprEval(*this, row, stats).value(root_);
}

// --- 5. Dump ------------------------------------------------------------------

namespace {

const char* opText(Expression::Op op) {
    switch (op) {
        case Expression::Op::Eq: return "=";  case Expression::Op::Ne: return "<>";
        case Expression::Op::Lt: return "<";  case Expression::Op::Le: return "<=";
        case Expression::Op::Gt: return ">";  case Expression::Op::Ge: return ">=";
    }
    return "?";
}

std::string dumpNode(const std::vector<Node>& nodes, int i) {
    const Node& n = nodes[i];
    auto kid = [&](std::size_t k) { return dumpNode(nodes, n.kids[k]); };
    const std::string no = n.negate ? "NOT " : "";
    switch (n.kind) {
        case Kind::Field:   return n.text;
        case Kind::Number:  return numberText(n.number);
        case Kind::Text:    return "'" + n.text + "'";
        case Kind::Arith:   return std::string("(") + n.arith + " " + kid(0) + " " + kid(1) + ")";
        case Kind::Neg:     return "(NEG " + kid(0) + ")";
        case Kind::Func:    return "(" + n.text + " " + kid(0) + ")";
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

std::string Expression::dump() const { return root_ < 0 ? "" : dumpNode(nodes_, root_); }

} // namespace r2d
