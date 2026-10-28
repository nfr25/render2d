// SPDX-License-Identifier: AGPL-3.0-or-later
// Expressions: reading, priorities, three-valued logic, text and numbers,
// arithmetic. The cases of notions §28, §29 and §36, on hand-written rows (no GDAL).

#include <cmath>
#include <string>
#include <vector>

#include "check.hpp"
#include "style/expression.hpp"

using namespace r2d;

namespace {

// Seven real addresses of Evere (the demo of notions §28).
const std::vector<FieldDef> kFields = {
    {"POLICENUM", FieldType::Text}, {"BOXNUMBER", FieldType::Text}, {"PARENTID", FieldType::Text},
    {"ZIPCODE", FieldType::Text},   {"CARTOANGLE", FieldType::Real}, {"STRNAMEFRE", FieldType::Text},
};
const Value N = std::monostate{};
Value t(const char* s) { return std::string(s); }

const std::vector<Attributes> kRows = {
    {t("106"), N,       N,          t("1140"), 283.19, t("Rue de Paris")},
    {t("106"), t("B002"), t(".../979654"), t("1140"), 283.19, t("Rue de Paris")},
    {t("106"), t("B003"), t(".../979654"), t("1140"), 283.19, t("Rue de Paris")},
    {t("75B"), N,       N,          t("1140"), 301.65, t("Avenue des Anciens Combattants")},
    {t("75B"), t("12"), t(".../43960"), t("1140"), 301.65, t("Avenue des Anciens Combattants")},
    {t("68"),  N,       N,          t("1140"), 272.75, t("Clos de l'Oasis")},
    {t("140"), N,       N,          t("1030"), 31.68,  t("Avenue Frans Courtens")},
};

Expression make(const std::string& text, bool bindRows = true) {
    std::string error;
    auto f = Expression::parseCondition(text, error);
    if (!f) {
        std::printf("  cannot parse [%s]: %s\n", text.c_str(), error.c_str());
        CHECK(false);
        return *Expression::parseCondition("1 = 1", error);
    }
    if (bindRows) CHECK(f->bind(kFields).empty());   // every name is a column of kRows
    return *f;
}

// One letter per row: T, F or ? (unknown).
std::string results(const std::string& text, EvalStats* stats = nullptr) {
    Expression f = make(text);
    EvalStats local;
    std::string out;
    for (const auto& row : kRows) {
        const Tri r = f.test(row, stats ? *stats : local);
        out += r == Tri::True ? 'T' : r == Tri::False ? 'F' : '?';
    }
    return out;
}

std::string dump(const std::string& text) { return make(text, false).dump(); }

std::string error(const std::string& text) {
    std::string e;
    return Expression::parseCondition(text, e) ? "" : e;
}

// §29: priorities, NOT > AND > OR, and parentheses.
void testPriorities() {
    CHECK(dump("AREA >= 30 AND AREA < 100") == "(AND (>= AREA 30) (< AREA 100))");
    CHECK(dump("a = 1 OR b = 2 AND c = 3") == "(OR (= a 1) (AND (= b 2) (= c 3)))");
    CHECK(dump("(a = 1 OR b = 2) AND c = 3") == "(AND (OR (= a 1) (= b 2)) (= c 3))");
    CHECK(dump("NOT a = 1 AND b = 2") == "(AND (NOT (= a 1)) (= b 2))");
    CHECK(dump("x between 1 and 2") == "(BETWEEN x 1 2)");             // keywords in any case
    CHECK(dump("Z NOT IN ('1030', '1140')") == "(NOT IN Z '1030' '1140')");
    CHECK(dump("S LIKE 'Rue%'") == "(LIKE S 'Rue%')");
    CHECK(dump("P IS NOT NULL") == "(IS NOT NULL P)");
    CHECK(dump("\"my field\" > -2.5") == "(> my field -2.5)");
}

// §28: the demo's buttons, row by row.
void testDemoCases() {
    CHECK(results("PARENTID IS NULL") == "TFFTFTT");
    EvalStats st;
    CHECK(results("POLICENUM < 80", &st) == "FFF??TF");     // '75B' is not a number
    CHECK(st.notNumber == 2);
    CHECK(results("POLICENUM < '80'") == "TTTTTTT");        // text order: '106' < '80'
    CHECK(results("NOT (BOXNUMBER LIKE 'B%')") == "?FF?T??");
    CHECK(results("ZIPCODE = '1140' AND CARTOANGLE BETWEEN 270 AND 300") == "TTTFFTF");
    CHECK(results("STRNAMEFRE = 'Clos de l''Oasis'") == "FFFFFTF");   // doubled apostrophe
}

// §28: three values. NULL makes a comparison unknown; AND/OR as in SQL.
void testThreeValued() {
    // Row 0 has BOXNUMBER NULL.
    CHECK(results("BOXNUMBER = 'B002'")[0] == '?');
    CHECK(results("NOT (BOXNUMBER = 'B002')")[0] == '?');
    CHECK(results("BOXNUMBER = 'B002' AND ZIPCODE = '9999'")[0] == 'F');   // false wins in AND
    CHECK(results("BOXNUMBER = 'B002' OR ZIPCODE = '1140'")[0] == 'T');    // true wins in OR
    CHECK(results("BOXNUMBER = 'B002' OR ZIPCODE = '9999'")[0] == '?');
    CHECK(results("BOXNUMBER IN ('B002', 'B003')") == "?TT?F??");
    CHECK(results("POLICENUM IN (68, 140)") == "FFF??TT");
}

void testNumbers() {
    CHECK(results("CARTOANGLE > 300") == "FFFTTFF");
    CHECK(results("CARTOANGLE BETWEEN 283.19 AND 283.19") == "TTTFFFF");   // bounds included
    CHECK(results("ZIPCODE >= 1100") == "TTTTTTF");                        // '1140' read as 1140
    CHECK(results("POLICENUM <> 106") == "FFF??TT");
}

void testErrors() {
    CHECK(error("AREA << 30").find("column 7") == 0);
    CHECK(error("AREA <").find("end of the expression") != std::string::npos);
    CHECK(!error("S = 'open").empty());
    CHECK(!error("a = 1 b = 2").empty());
    CHECK(!error("a NOT = 1").empty());
    CHECK(!error("AREA").empty());                  // a value is not a condition
    CHECK(!error("x IN (1, 2").empty());
    CHECK(error("PARENTID IS NULL").empty());
}

void testBinding() {
    std::string e;
    auto f = Expression::parseCondition("SURFACE > 30 OR AREA < 3 OR AREA > 9", e);
    if (!f) { CHECK(false); return; }
    const std::vector<FieldDef> fields = {{"ID", FieldType::Text}, {"AREA", FieldType::Real}};
    const auto missing = f->bind(fields);
    CHECK(missing.size() == 1 && missing[0] == "SURFACE");
    CHECK(f->columns() == std::vector<int>({1, 1}));
    CHECK(f->fieldNames() == std::vector<std::string>({"SURFACE", "AREA"}));
    EvalStats st;
    CHECK(f->test({std::string("x"), 2.0}, st) == Tri::True);      // SURFACE unknown, AREA < 3 true
    CHECK(f->test({std::string("x"), 5.0}, st) == Tri::Unknown);   // unknown OR false OR false
}

// §36: arithmetic, with the usual priorities, and values.
double num(const std::string& text, const Attributes& row = {}) {
    std::string e;
    auto x = Expression::parseValue(text, e);
    if (!x) { std::printf("  cannot parse [%s]: %s\n", text.c_str(), e.c_str()); CHECK(false); return -1e9; }
    x->bind(kFields);
    EvalStats st;
    const Value v = x->value(row, st);
    if (const auto* d = std::get_if<double>(&v)) return *d;
    return std::nan("");
}

void testArithmetic() {
    CHECK_NEAR(num("360 - 3 * 2"), 354.0, 0.0);           // * before -
    CHECK_NEAR(num("(360 - 3) * 2"), 714.0, 0.0);
    CHECK_NEAR(num("360-3"), 357.0, 0.0);                 // not 360 followed by -3
    CHECK_NEAR(num("-2 * -3"), 6.0, 0.0);
    CHECK_NEAR(num("10 / 4"), 2.5, 0.0);
    CHECK(std::isnan(num("1 / 0")));                       // NULL, not infinity
    CHECK_NEAR(num("360 - CARTOANGLE", kRows[0]), 360.0 - 283.19, 1e-9);   // rue de Paris 106
    CHECK_NEAR(num("POLICENUM + 1", kRows[0]), 107.0, 0.0);              // '106' read as a number
    CHECK(std::isnan(num("POLICENUM + 1", kRows[3])));                     // '75B': NULL
    CHECK(dump("AREA / 10000 > 1") == "(> (/ AREA 10000) 1)");
    CHECK(dump("-AREA < 3") == "(< (NEG AREA) 3)");
    CHECK(results("CARTOANGLE - 280 BETWEEN 0 AND 10") == "TTTFFFF");
    // A value is not a condition, and a condition is not a value operand.
    CHECK(!error("CARTOANGLE + 1").empty());
    CHECK(!error("(a = 1) + 2 > 0").empty());

    std::string e;
    auto label = Expression::parseValue("POLICENUM", e);
    CHECK(label.has_value());
    if (label) {
        label->bind(kFields);
        EvalStats st;
        CHECK(std::get<std::string>(label->value(kRows[3], st)) == "75B");
        CHECK(isNull(label->value({}, st)));               // missing row: NULL
    }
    EvalStats st;
    CHECK(std::get<std::string>(Expression::literalText("Evere").value({}, st)) == "Evere");

    // §41: functions.
    CHECK_NEAR(num("4 * sqrt(4)"), 8.0, 0.0);              // a door with 4 flats: 8 pixels
    CHECK_NEAR(num("4 * SQRT(100)"), 40.0, 0.0);
    CHECK(std::isnan(num("sqrt(-1)")));                    // NULL
    CHECK_NEAR(num("abs(90 - 319.32)"), 229.32, 1e-9);
    CHECK(dump("sqrt(AREA) > 10") == "(> (SQRT AREA) 10)");
    CHECK(!error("cbrt(AREA) > 1").empty());               // unknown function
}

} // namespace

int main() {
    testPriorities();
    testDemoCases();
    testThreeValued();
    testNumbers();
    testErrors();
    testBinding();
    testArithmetic();
    return check::summary();
}
