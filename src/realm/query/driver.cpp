#include "realm/query/driver.hpp"
#include "realm/query/query_bison.hpp"
#include <realm/parser/query_builder.hpp>
#include <realm/decimal128.hpp>
#include <realm/uuid.hpp>

using namespace realm;
using namespace std::string_literals;

// Whether to generate parser debug traces.
static bool trace_parsing = false;
// Whether to generate scanner debug traces.
static bool trace_scanning = true;

int ParserDriver::parse(const std::string& str)
{
    std::cout << str << std::endl;
    parse_string = str;
    std::string dummy;
    location.initialize(&dummy);
    scan_begin(trace_scanning);
    yy::parser parse(*this);
    parse.set_debug_level(trace_parsing);
    int res = parse();
    scan_end();
    if (parse_error) {
        throw std::runtime_error(error_string);
    }
    return res;
}

namespace {

class MixedArguments : public query_builder::Arguments {
public:
    MixedArguments(const std::vector<Mixed>& args)
        : m_args(args)
    {
    }
    bool bool_for_argument(size_t n) final
    {
        return m_args.at(n).get<bool>();
    }
    long long long_for_argument(size_t n) final
    {
        return m_args.at(n).get<int64_t>();
    }
    float float_for_argument(size_t n) final
    {
        return m_args.at(n).get<float>();
    }
    double double_for_argument(size_t n) final
    {
        return m_args.at(n).get<double>();
    }
    StringData string_for_argument(size_t n) final
    {
        return m_args.at(n).get<StringData>();
    }
    BinaryData binary_for_argument(size_t n) final
    {
        return m_args.at(n).get<BinaryData>();
    }
    Timestamp timestamp_for_argument(size_t n) final
    {
        return m_args.at(n).get<Timestamp>();
    }
    ObjectId objectid_for_argument(size_t n) final
    {
        return m_args.at(n).get<ObjectId>();
    }
    UUID uuid_for_argument(size_t n) final
    {
        return m_args.at(n).get<UUID>();
    }
    Decimal128 decimal128_for_argument(size_t n) final
    {
        return m_args.at(n).get<Decimal128>();
    }
    ObjKey object_index_for_argument(size_t n) final
    {
        return m_args.at(n).get<ObjKey>();
    }
    bool is_argument_null(size_t n) final
    {
        return m_args.at(n).is_null();
    }

private:
    const std::vector<Mixed>& m_args;
};

} // namespace

namespace realm {

namespace query_parser {
Timestamp get_timestamp_if_valid(int64_t seconds, int32_t nanoseconds)
{
    const bool both_non_negative = seconds >= 0 && nanoseconds >= 0;
    const bool both_non_positive = seconds <= 0 && nanoseconds <= 0;
    if (both_non_negative || both_non_positive) {
        return Timestamp(seconds, nanoseconds);
    }
    throw std::runtime_error("Invalid timestamp format");
}

ParserNode::~ParserNode() {}


AtomPredNode::~AtomPredNode() {}

std::any NotNode::visit(ParserDriver* drv)
{
    Query query = std::any_cast<Query>(atom_pred->visit(drv));
    Query q = drv->m_base_table->where();
    q.Not();
    q.and_query(query);
    return {q};
}

std::any ParensNode::visit(ParserDriver* drv)
{
    return pred->visit(drv);
}

std::any OrNode::visit(ParserDriver* drv)
{
    if (and_preds.size() == 1) {
        return and_preds[0]->visit(drv);
    }
    auto it = and_preds.begin();
    auto q = std::any_cast<Query>((*it)->visit(drv));
    q.Or();

    ++it;
    while (it != and_preds.end()) {
        q.and_query(std::move(std::any_cast<Query>((*it)->visit(drv))));
        ++it;
    }
    return q;
}

std::any AndNode::visit(ParserDriver* drv)
{
    if (atom_preds.size() == 1) {
        return atom_preds[0]->visit(drv);
    }
    Query q(drv->m_base_table);
    for (auto it : atom_preds) {
        q.and_query(std::move(std::any_cast<Query>(it->visit(drv))));
    }
    return q;
}

std::any EqualitylNode::visit(ParserDriver* drv)
{
    auto [left, right] = drv->cmp(values);

    const TableProperty* prop = dynamic_cast<const TableProperty*>(left.get());
    if (prop && !prop->links_exist() && right->has_constant_evaluation() && left->get_type() == right->get_type()) {
        auto col_key = prop->column_key();
        Mixed val = right->get_mixed();
        if (val.is_null()) {
            switch (op) {
                case CompareNode::EQUAL:
                    return drv->m_base_table->where().equal(col_key, realm::null());
                case CompareNode::NOT_EQUAL:
                    return drv->m_base_table->where().not_equal(col_key, realm::null());
            }
        }
        switch (left->get_type()) {
            case type_Int:
                return drv->simple_query(op, col_key, val.get_int());
            case type_Bool:
                return drv->simple_query(op, col_key, val.get_bool());
            case type_String:
                break;
            case type_Binary:
                break;
            case type_Timestamp:
                return drv->simple_query(op, col_key, val.get<Timestamp>());
            case type_Float:
                return drv->simple_query(op, col_key, val.get_float());
                break;
            case type_Double:
                return drv->simple_query(op, col_key, val.get_double());
                break;
            case type_Decimal:
                return drv->simple_query(op, col_key, val.get<Decimal128>());
                break;
            case type_ObjectId:
                break;
            case type_UUID:
                return drv->simple_query(op, col_key, val.get<UUID>());
                break;
            default:
                break;
        }
    }
    if (case_sensitive) {
        switch (op) {
            case CompareNode::EQUAL:
                return Query(std::unique_ptr<Expression>(new Compare<Equal>(std::move(right), std::move(left))));
            case CompareNode::NOT_EQUAL:
                return Query(std::unique_ptr<Expression>(new Compare<NotEqual>(std::move(right), std::move(left))));
        }
    }
    else {
        switch (op) {
            case CompareNode::EQUAL:
                return Query(std::unique_ptr<Expression>(new Compare<EqualIns>(std::move(right), std::move(left))));
            case CompareNode::NOT_EQUAL:
                return Query(
                    std::unique_ptr<Expression>(new Compare<NotEqualIns>(std::move(right), std::move(left))));
        }
    }
    return {};
}

static std::map<int, std::string> opstr = {
    {CompareNode::GREATER, ">"},
    {CompareNode::LESS, "<"},
    {CompareNode::GREATER_EQUAL, ">="},
    {CompareNode::LESS_EQUAL, "<="},
    {CompareNode::BEGINSWITH, "beginswith"},
    {CompareNode::ENDSWITH, "endswith"},
    {CompareNode::CONTAINS, "contains"},
    {CompareNode::LIKE, "like"},
};

std::any RelationalNode::visit(ParserDriver* drv)
{
    auto [left, right] = drv->cmp(values);

    if (left->get_type() == type_UUID) {
        throw std::logic_error(util::format(
            "Unsupported operator %1 in query. Only equal (==) and not equal (!=) are supported for this type.",
            opstr[op]));
    }

    const TableProperty* prop = dynamic_cast<const TableProperty*>(left.get());
    if (prop && !prop->links_exist() && right->has_constant_evaluation() && left->get_type() == right->get_type()) {
        auto col_key = prop->column_key();
        switch (left->get_type()) {
            case type_Int:
                return drv->simple_query(op, col_key, right->get_mixed().get_int());
            case type_Bool:
                break;
            case type_String:
                break;
            case type_Binary:
                break;
            case type_Timestamp:
                return drv->simple_query(op, col_key, right->get_mixed().get<Timestamp>());
            case type_Float:
                break;
            case type_Double:
                break;
            case type_Decimal:
                return drv->simple_query(op, col_key, right->get_mixed().get<Decimal128>());
                break;
            case type_ObjectId:
                break;
            case type_UUID:
                break;
            default:
                break;
        }
    }
    switch (op) {
        case CompareNode::GREATER:
            return Query(std::unique_ptr<Expression>(new Compare<Less>(std::move(right), std::move(left))));
        case CompareNode::LESS:
            return Query(std::unique_ptr<Expression>(new Compare<Greater>(std::move(right), std::move(left))));
        case CompareNode::GREATER_EQUAL:
            return Query(std::unique_ptr<Expression>(new Compare<LessEqual>(std::move(right), std::move(left))));
        case CompareNode::LESS_EQUAL:
            return Query(std::unique_ptr<Expression>(new Compare<GreaterEqual>(std::move(right), std::move(left))));
    }
    return {};
}

std::any StringOpsNode::visit(ParserDriver* drv)
{
    auto [left, right] = drv->cmp(values);

    if (left->get_type() == type_UUID) {
        throw std::logic_error(util::format(
            "Unsupported operator %1 in query. Only equal (==) and not equal (!=) are supported for this type.",
            opstr[op]));
    }

    auto string_expr = dynamic_cast<ConstantStringValue*>(right.get());
    std::string val = string_expr->get_mixed().get_string();

    const TableProperty* prop = dynamic_cast<const TableProperty*>(left.get());
    if (prop && !prop->links_exist() && left->get_type() == type_String) {
        auto col_key = prop->column_key();
        switch (op) {
            case CompareNode::BEGINSWITH:
                return drv->m_base_table->where().begins_with(col_key, val, case_sensitive);
            case CompareNode::ENDSWITH:
                return drv->m_base_table->where().ends_with(col_key, val, case_sensitive);
            case CompareNode::CONTAINS:
                return drv->m_base_table->where().contains(col_key, val, case_sensitive);
            case CompareNode::LIKE:
                return drv->m_base_table->where().like(col_key, val, case_sensitive);
        }
    }

    if (case_sensitive) {
        switch (op) {
            case CompareNode::BEGINSWITH:
                return Query(std::unique_ptr<Expression>(new Compare<BeginsWith>(std::move(right), std::move(left))));
            case CompareNode::ENDSWITH:
                return Query(std::unique_ptr<Expression>(new Compare<EndsWith>(std::move(right), std::move(left))));
            case CompareNode::CONTAINS:
                return Query(std::unique_ptr<Expression>(new Compare<Contains>(std::move(right), std::move(left))));
            case CompareNode::LIKE:
                return Query(std::unique_ptr<Expression>(new Compare<Like>(std::move(right), std::move(left))));
        }
    }
    else {
        switch (op) {
            case CompareNode::BEGINSWITH:
                return Query(
                    std::unique_ptr<Expression>(new Compare<BeginsWithIns>(std::move(right), std::move(left))));
            case CompareNode::ENDSWITH:
                return Query(
                    std::unique_ptr<Expression>(new Compare<EndsWithIns>(std::move(right), std::move(left))));
            case CompareNode::CONTAINS:
                return Query(
                    std::unique_ptr<Expression>(new Compare<ContainsIns>(std::move(right), std::move(left))));
            case CompareNode::LIKE:
                return Query(std::unique_ptr<Expression>(new Compare<LikeIns>(std::move(right), std::move(left))));
        }
    }
    return {};
}

std::any ValueNode::visit(ParserDriver* drv)
{
    if (prop) {
        return prop->visit(drv);
    }
    REALM_ASSERT(constant);
    return constant->visit(drv);
}

std::any PropNode::visit(ParserDriver* drv)
{
    drv->push(comp_type);
    Subexpr* subexpr = std::any_cast<LinkChain>(path->visit(drv)).column(identifier);

    if (post_op) {
        drv->push(subexpr);
        return post_op->visit(drv);
    }
    return subexpr;
}

std::any PostOpNode::visit(ParserDriver* drv)
{
    std::unique_ptr<realm::Subexpr> subexpr(std::get<Subexpr*>(drv->pop()));
    if (auto s = dynamic_cast<Columns<Link>*>(subexpr.get())) {
        return s->count().clone().release();
    }
    if (auto s = dynamic_cast<ColumnListBase*>(subexpr.get())) {
        return s->size().clone().release();
    }
    if (auto s = dynamic_cast<Columns<StringData>*>(subexpr.get())) {
        return s->size().clone().release();
    }

    REALM_UNREACHABLE();
    return {};
}

std::any LinkAggrNode::visit(ParserDriver* drv)
{
    drv->push(ExpressionComparisonType::Any);
    auto link_chain = std::any_cast<LinkChain>(path->visit(drv));
    auto subexpr = std::unique_ptr<Subexpr>(link_chain.column(link));
    auto link_prop = dynamic_cast<Columns<Link>*>(subexpr.get());
    if (!link_prop) {
        std::string msg = "Property '"s + link + "' is not a linklist"s;
        throw std::runtime_error(msg);
    }
    auto col_key = link_chain.get_current_table()->get_column_key(prop);

    Subexpr* sub_column;
    switch (col_key.get_type()) {
        case type_Float:
            sub_column = link_prop->column<float>(col_key).clone().release();
            break;
        case type_Double:
            sub_column = link_prop->column<double>(col_key).clone().release();
            break;
        case type_Decimal:
            sub_column = link_prop->column<Decimal>(col_key).clone().release();
            break;
        default:
            break;
    }
    drv->push(sub_column);
    return aggr_op->visit(drv);
}

std::any ListAggrNode::visit(ParserDriver* drv)
{
    drv->push(ExpressionComparisonType::Any);
    auto link_chain = std::any_cast<LinkChain>(path->visit(drv));
    auto subexpr = link_chain.column(identifier);
    drv->push(subexpr);
    return aggr_op->visit(drv);
}

std::any AggrNode::visit(ParserDriver* drv)
{
    auto subexpr = std::get<Subexpr*>(drv->pop());
    if (auto list_prop = dynamic_cast<ColumnListBase*>(subexpr)) {
        switch (type) {
            case MAX:
                return list_prop->max_of().release();
                break;
            case MIN:
                return list_prop->min_of().release();
                break;
            case SUM:
                return list_prop->sum_of().release();
                break;
            case AVG:
                return list_prop->avg_of().release();
                break;
        }
    }


    if (auto prop = dynamic_cast<SubColumnBase*>(subexpr)) {
        switch (type) {
            case MAX:
                return prop->max_of().release();
                break;
            case MIN:
                return prop->min_of().release();
                break;
            case SUM:
                return prop->sum_of().release();
                break;
            case AVG:
                return prop->avg_of().release();
                break;
        }
    }

    REALM_UNREACHABLE();
    return {};
}

std::any ConstantNode::visit(ParserDriver* drv)
{
    Subexpr* ret = nullptr;
    auto hint = std::get<DataType>(drv->pop());
    switch (type) {
        case Type::NUMBER:
        case Type::FLOAT: {
            if (hint == type_Decimal) {
                Decimal128 d(text);
                ret = new Value<Decimal128>(d);
            }
            else {
                if (text.find_first_of(".eE") < text.length()) {
                    double d = strtod(text.c_str(), nullptr);
                    ret = new Value<double>(d);
                }
                else {
                    int64_t n = strtol(text.c_str(), nullptr, 0);
                    ret = new Value<int64_t>(n);
                }
            }
            break;
        }
        case Type::STRING: {
            std::string str = text.substr(1, text.size() - 2);
            ret = new ConstantStringValue(str);
            break;
        }
        case Type::TIMESTAMP: {
            auto s = text;
            int64_t seconds;
            int32_t nanoseconds;
            if (s[0] == 'T') {
                size_t colon_pos = s.find(":");
                std::string s1 = s.substr(1, colon_pos - 1);
                std::string s2 = s.substr(colon_pos + 1);
                seconds = strtol(s1.c_str(), nullptr, 0);
                nanoseconds = strtol(s2.c_str(), nullptr, 0);
            }
            else {
                // readable format YYYY-MM-DD-HH:MM:SS:NANOS nanos optional
                struct tm tmp = tm();
                char sep = s.find("@") < s.size() ? '@' : 'T';
                std::string fmt = "%d-%d-%d"s + sep + "%d:%d:%d:%d"s;
                int cnt = sscanf(s.c_str(), fmt.c_str(), &tmp.tm_year, &tmp.tm_mon, &tmp.tm_mday, &tmp.tm_hour,
                                 &tmp.tm_min, &tmp.tm_sec, &nanoseconds);
                REALM_ASSERT(cnt >= 6);
                tmp.tm_year -= 1900; // epoch offset (see man mktime)
                tmp.tm_mon -= 1;     // converts from 1-12 to 0-11

                if (tmp.tm_year < 0) {
                    // platform timegm functions do not throw errors, they return -1 which is also a valid time
                    throw std::logic_error("Conversion of dates before 1900 is not supported.");
                }

                seconds = platform_timegm(tmp); // UTC time
                if (cnt == 6) {
                    nanoseconds = 0;
                }
                if (nanoseconds < 0) {
                    throw std::logic_error("The nanoseconds of a Timestamp cannot be negative.");
                }
                if (seconds < 0) { // seconds determines the sign of the nanoseconds part
                    nanoseconds *= -1;
                }
            }
            ret = new Value<Timestamp>(get_timestamp_if_valid(seconds, nanoseconds));
            break;
        }
        case Type::UUID_T:
            ret = new Value<UUID>(UUID(text.substr(5, text.size() - 6)));
            break;
        case Type::OID:
            ret = new Value<ObjectId>(ObjectId(text.substr(4, text.size() - 5).c_str()));
            break;
        case Type::NULL_VAL:
            ret = new Value<null>(realm::null());
            break;
        case Type::TRUE:
            ret = new Value<Bool>(true);
            break;
        case Type::FALSE:
            ret = new Value<Bool>(false);
            break;
        case Type::ARG: {
            size_t arg_no = size_t(strtol(text.substr(1).c_str(), nullptr, 10));
            if (drv->m_args.is_argument_null(arg_no)) {
                ret = new Value<null>(realm::null());
            }
            else {
                switch (hint) {
                    case type_Int:
                        ret = new Value<int64_t>(drv->m_args.long_for_argument(arg_no));
                        break;
                    case type_String:
                        ret = new ConstantStringValue(drv->m_args.string_for_argument(arg_no));
                        break;
                    case type_Bool:
                        ret = new Value<Bool>(drv->m_args.bool_for_argument(arg_no));
                        break;
                    case type_Float:
                        ret = new Value<float>(drv->m_args.float_for_argument(arg_no));
                        break;
                    case type_Double:
                        ret = new Value<double>(drv->m_args.double_for_argument(arg_no));
                        break;
                    case type_Timestamp:
                        ret = new Value<Timestamp>(drv->m_args.timestamp_for_argument(arg_no));
                        break;
                    case type_Decimal:
                        ret = new Value<Decimal128>(drv->m_args.decimal128_for_argument(arg_no));
                        break;
                    case type_UUID:
                        ret = new Value<UUID>(drv->m_args.uuid_for_argument(arg_no));
                        break;
                    case type_ObjectId:
                        ret = new Value<ObjectId>(drv->m_args.objectid_for_argument(arg_no));
                        break;
                    default:
                        break;
                }
            }
            break;
        }
    }
    REALM_ASSERT(ret);
    return ret;
}

std::any TrueOrFalseNode::visit(ParserDriver* drv)
{
    Query q = drv->m_base_table->where();
    if (true_or_false) {
        q.and_query(std::unique_ptr<realm::Expression>(new TrueExpression));
    }
    else {
        q.and_query(std::unique_ptr<realm::Expression>(new FalseExpression));
    }
    return q;
}

std::any PathNode::visit(ParserDriver* drv)
{
    auto comp_type = std::get<ExpressionComparisonType>(drv->pop());
    LinkChain link_chain(drv->m_base_table, comp_type);
    for (auto path_elem : path_elems) {
        link_chain.link(path_elem);
    }
    return link_chain;
}

std::pair<std::unique_ptr<Subexpr>, std::unique_ptr<Subexpr>> ParserDriver::cmp(const std::vector<ValueNode*>& values)
{
    std::unique_ptr<Subexpr> left;
    std::unique_ptr<Subexpr> right;

    auto left_constant = values[0]->constant;
    auto right_constant = values[1]->constant;
    auto left_prop = values[0]->prop;
    auto right_prop = values[1]->prop;

    if (left_constant && right_constant) {
        throw std::runtime_error("Cannot compare two constants");
    }

    if (right_constant) {
        // Take left first - it cannot be a constant
        left.reset(std::any_cast<Subexpr*>(left_prop->visit(this)));
        push(left->get_type());
        right.reset(std::any_cast<Subexpr*>(right_constant->visit(this)));
    }
    else {
        right.reset(std::any_cast<Subexpr*>(right_prop->visit(this)));
        if (left_constant) {
            push(right->get_type());
            left.reset(std::any_cast<Subexpr*>(left_constant->visit(this)));
        }
        else {
            left.reset(std::any_cast<Subexpr*>(left_prop->visit(this)));
        }
    }
    return {std::move(left), std::move(right)};
}

} // namespace query_parser

Query Table::query(const std::string& query_string, const std::vector<Mixed>& arguments) const
{
    MixedArguments args(arguments);
    return query(query_string, args, {});
}

Query Table::query(const std::string& query_string, query_builder::Arguments& args,
                   const parser::KeyPathMapping&) const
{
    ParserDriver driver(m_own_ref, args);
    driver.parse(query_string);
    return std::any_cast<Query>(driver.result->visit(&driver));
}

Subexpr* LinkChain::column(std::string col)
{
    auto col_key = m_current_table->get_column_key(col);
    if (!col_key) {
        std::string err = m_current_table->get_name();
        err += " has no property: ";
        err += col;
        throw std::runtime_error(err);
    }

    if (m_current_table->is_list(col_key)) {
        switch (col_key.get_type()) {
            case col_type_Int:
                return new Columns<Lst<Int>>(col_key, m_base_table, m_link_cols, m_comparison_type);
            case col_type_Bool:
                return new Columns<Lst<Bool>>(col_key, m_base_table, m_link_cols, m_comparison_type);
            case col_type_String:
                return new Columns<Lst<String>>(col_key, m_base_table, m_link_cols, m_comparison_type);
            case col_type_Float:
                return new Columns<Lst<Float>>(col_key, m_base_table, m_link_cols, m_comparison_type);
            case col_type_Double:
                return new Columns<Lst<Double>>(col_key, m_base_table, m_link_cols, m_comparison_type);
            case col_type_Timestamp:
                return new Columns<Lst<Timestamp>>(col_key, m_base_table, m_link_cols, m_comparison_type);
            case col_type_Decimal:
                return new Columns<Lst<Decimal>>(col_key, m_base_table, m_link_cols, m_comparison_type);
            case col_type_UUID:
                return new Columns<Lst<Timestamp>>(col_key, m_base_table, m_link_cols, m_comparison_type);
            case col_type_ObjectId:
                return new Columns<Lst<ObjectId>>(col_key, m_base_table, m_link_cols, m_comparison_type);
            case col_type_Mixed:
                return new Columns<Lst<Mixed>>(col_key, m_base_table, m_link_cols, m_comparison_type);
            case col_type_LinkList:
                add(col_key);
                return new Columns<Link>(col_key, m_base_table, m_link_cols, m_comparison_type);
            default:
                break;
        }
    }
    else {
        switch (col_key.get_type()) {
            case col_type_Int:
                return new Columns<Int>(col_key, m_base_table, m_link_cols);
            case col_type_Bool:
                return new Columns<Bool>(col_key, m_base_table, m_link_cols);
            case col_type_String:
                return new Columns<String>(col_key, m_base_table, m_link_cols);
            case col_type_Float:
                return new Columns<Float>(col_key, m_base_table, m_link_cols);
            case col_type_Double:
                return new Columns<Double>(col_key, m_base_table, m_link_cols);
            case col_type_Timestamp:
                return new Columns<Timestamp>(col_key, m_base_table, m_link_cols);
            case col_type_Decimal:
                return new Columns<Decimal128>(col_key, m_base_table, m_link_cols);
            case col_type_UUID:
                return new Columns<UUID>(col_key, m_base_table, m_link_cols);
            case col_type_ObjectId:
                return new Columns<ObjectId>(col_key, m_base_table, m_link_cols);
            case col_type_Mixed:
                return new Columns<Mixed>(col_key, m_base_table, m_link_cols);
            case col_type_Link:
                return new Columns<ObjKey>(col_key, m_base_table, m_link_cols);
            default:
                break;
        }
    }
    REALM_UNREACHABLE();
    return nullptr;
}
} // namespace realm
