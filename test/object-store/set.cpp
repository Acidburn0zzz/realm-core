#include "catch2/catch.hpp"

#include "collection_fixtures.hpp"
#include "util/test_file.hpp"
#include "util/index_helpers.hpp"

#include <realm/object-store/binding_context.hpp>
#include <realm/object-store/set.hpp>
#include <realm/object-store/object.hpp>
#include <realm/object-store/object_schema.hpp>
#include <realm/object-store/property.hpp>
#include <realm/object-store/results.hpp>
#include <realm/object-store/schema.hpp>

#include <realm/object-store/impl/realm_coordinator.hpp>
#include <realm/object-store/impl/object_accessor_impl.hpp>

#include <realm/db.hpp>
#include <realm/util/any.hpp>
#include <realm/version.hpp>

using namespace realm;
using namespace realm::util;
namespace cf = realm::collection_fixtures;

TEMPLATE_TEST_CASE("set all types", "[set]", cf::MixedVal, cf::Int, cf::Bool, cf::Float, cf::Double, cf::String,
                   cf::Binary, cf::Date, cf::OID, cf::Decimal, cf::UUID, cf::BoxedOptional<cf::Int>,
                   cf::BoxedOptional<cf::Bool>, cf::BoxedOptional<cf::Float>, cf::BoxedOptional<cf::Double>,
                   cf::BoxedOptional<cf::OID>, cf::BoxedOptional<cf::UUID>, cf::UnboxedOptional<cf::String>,
                   cf::UnboxedOptional<cf::Binary>, cf::UnboxedOptional<cf::Date>, cf::UnboxedOptional<cf::Decimal>)
{
    using T = typename TestType::Type;
    using Boxed = typename TestType::Boxed;
    using W = typename TestType::Wrapped;

    InMemoryTestFile config;
    config.automatic_change_notifications = false;
    auto r = Realm::get_shared_realm(config);
    r->update_schema({
        {"table",
         {{"value_set", PropertyType::Set | TestType::property_type()},
          {"link_set", PropertyType::Set | PropertyType::Object, "table2"}}},
        {"table2", {{"id", PropertyType::Int, Property::IsPrimary{true}}}},
    });
    auto table = r->read_group().get_table("class_table");
    ColKey col_set = table->get_column_key("value_set");

    auto write = [&](auto&& f) {
        r->begin_transaction();
        if constexpr (std::is_void_v<decltype(f())>) {
            f();
            r->commit_transaction();
            advance_and_notify(*r);
        }
        else {
            auto result = f();
            r->commit_transaction();
            advance_and_notify(*r);
            return result;
        }
    };

    auto obj = write([&] {
        return table->create_object();
    });

    auto values = TestType::values();

    SECTION("basics") {
        object_store::Set set{r, obj, col_set};
        Results set_as_results = set.as_results();
        CppContext ctx(r);

        SECTION("valid") {
            REQUIRE(set.is_valid());
            REQUIRE_NOTHROW(set.verify_attached());
            object_store::Set unattached;
            REQUIRE_THROWS(unattached.verify_attached());
            REQUIRE(!unattached.is_valid());
        }

        SECTION("basic value operations") {
            REQUIRE(set.size() == 0);
            REQUIRE(set.get_type() == TestType::property_type());
            REQUIRE(set_as_results.get_type() == TestType::property_type());
            write([&]() {
                for (size_t i = 0; i < values.size(); ++i) {
                    auto result = set.insert(T(values[i]));
                    REQUIRE(result.first < values.size());
                    REQUIRE(result.second);
                    auto result2 = set.insert(T(values[i]));
                    REQUIRE(!result2.second);
                }
            });

            REQUIRE(set.is_valid());
            REQUIRE(set.size() == values.size());
            REQUIRE(set_as_results.size() == values.size());

            SECTION("get()") {
                std::vector<size_t> found_indices;
                for (auto val : values) {
                    size_t ndx = set.find(T(val));
                    REQUIRE(ndx < set.size());
                    found_indices.push_back(ndx);
                    size_t ndx_any = set.find_any(Mixed{T(val)});
                    REQUIRE(ndx_any == ndx);
                    REQUIRE(set.get<T>(ndx) == T(val));
                    REQUIRE(set.get_any(ndx) == Mixed{T(val)});
                    auto ctx_val = set.get(ctx, ndx);
                    REQUIRE(any_cast<Boxed>(ctx_val) == Boxed(T(val)));
                    // and through results
                    auto res_ndx = set_as_results.index_of(T(val));
                    REQUIRE(res_ndx == ndx);
                    REQUIRE(set_as_results.get<T>(res_ndx) == T(val));
                    auto res_ctx_val = set_as_results.get(ctx, res_ndx);
                    REQUIRE(any_cast<Boxed>(res_ctx_val) == Boxed(T(val)));
                    REQUIRE(set_as_results.get_any(res_ndx) == Mixed{T(val)});
                }
                // we do not require any particular ordering
                std::sort(begin(found_indices), end(found_indices), std::less<size_t>());
                std::vector<size_t> expected_indices(values.size());
                std::iota(begin(expected_indices), end(expected_indices), 0);
                REQUIRE(found_indices == expected_indices);
            }

            auto check_empty = [&]() {
                REQUIRE(set.size() == 0);
                for (size_t i = 0; i < values.size(); ++i) {
                    REQUIRE(set.find(T(values[i])) == realm::not_found);
                }
            };
            SECTION("remove()") {
                write([&]() {
                    for (size_t i = 0; i < values.size(); ++i) {
                        auto result = set.remove(T(values[i]));
                        REQUIRE(result.first < values.size());
                        REQUIRE(result.second);
                        auto result2 = set.remove(T(values[i]));
                        REQUIRE(!result2.second);
                    }
                });
                check_empty();
            }
            SECTION("remove_any()") {
                write([&]() {
                    for (size_t i = 0; i < values.size(); ++i) {
                        auto result = set.remove_any(Mixed(T(values[i])));
                        REQUIRE(result.first < values.size());
                        REQUIRE(result.second);
                        auto result2 = set.remove_any(Mixed(T(values[i])));
                        REQUIRE(!result2.second);
                    }
                });
                check_empty();
            }
            SECTION("remove(ctx)") {
                write([&]() {
                    for (size_t i = 0; i < values.size(); ++i) {
                        auto result = set.remove(ctx, TestType::to_any(T(values[i])));
                        REQUIRE(result.first < values.size());
                        REQUIRE(result.second);
                        auto result2 = set.remove(ctx, TestType::to_any(T(values[i])));
                        REQUIRE(!result2.second);
                    }
                });
                check_empty();
            }
            SECTION("remove_all()") {
                write([&]() {
                    set.remove_all();
                });
                check_empty();
            }
            SECTION("delete_all()") {
                write([&]() {
                    set.delete_all();
                });
                check_empty();
            }
            SECTION("Results::clear()") {
                write([&]() {
                    set_as_results.clear();
                });
                check_empty();
            }
            SECTION("min()") {
                if (!TestType::can_minmax()) {
                    REQUIRE_THROWS_AS(set.min(), Results::UnsupportedColumnTypeException);
                    REQUIRE_THROWS_AS(set_as_results.min(), Results::UnsupportedColumnTypeException);
                    return;
                }
                REQUIRE(Mixed(TestType::min()) == set.min());
                REQUIRE(Mixed(TestType::min()) == set_as_results.min());
                write([&]() {
                    set.remove_all();
                });
                REQUIRE(!set.min());
                REQUIRE(!set_as_results.min());
            }
            SECTION("max()") {
                if (!TestType::can_minmax()) {
                    REQUIRE_THROWS_AS(set.max(), Results::UnsupportedColumnTypeException);
                    REQUIRE_THROWS_AS(set_as_results.max(), Results::UnsupportedColumnTypeException);
                    return;
                }
                REQUIRE(Mixed(TestType::max()) == set.max());
                REQUIRE(Mixed(TestType::max()) == set_as_results.max());
                write([&]() {
                    set.remove_all();
                });
                REQUIRE(!set.max());
                REQUIRE(!set_as_results.max());
            }
            SECTION("sum()") {
                if (!TestType::can_sum()) {
                    REQUIRE_THROWS_AS(set.sum(), Results::UnsupportedColumnTypeException);
                    REQUIRE_THROWS_AS(set_as_results.sum(), Results::UnsupportedColumnTypeException);
                    return;
                }
                REQUIRE(cf::get<W>(set.sum()) == TestType::sum());
                REQUIRE(cf::get<W>(*set_as_results.sum()) == TestType::sum());
                write([&]() {
                    set.remove_all();
                });
                REQUIRE(set.sum() == 0);
                REQUIRE(set_as_results.sum() == 0);
            }
            SECTION("average()") {
                if (!TestType::can_average()) {
                    REQUIRE_THROWS_AS(set.average(), Results::UnsupportedColumnTypeException);
                    REQUIRE_THROWS_AS(set_as_results.average(), Results::UnsupportedColumnTypeException);
                    return;
                }
                REQUIRE(cf::get<typename TestType::AvgType>(*set.average()) == TestType::average());
                REQUIRE(cf::get<typename TestType::AvgType>(*set_as_results.average()) == TestType::average());
                write([&]() {
                    set.remove_all();
                });
                REQUIRE(!set.average());
                REQUIRE(!set_as_results.average());
            }
            SECTION("sort") {
                SECTION("ascending") {
                    auto sorted = set_as_results.sort({{"self", true}});
                    std::sort(begin(values), end(values), cf::less());
                    REQUIRE(sorted == values);
                }
                SECTION("descending") {
                    auto sorted = set_as_results.sort({{"self", false}});
                    std::sort(begin(values), end(values), cf::greater());
                    REQUIRE(sorted == values);
                }
            }
        }
    }
}

TEST_CASE("set", "[set]") {
    InMemoryTestFile config;
    config.automatic_change_notifications = false;
    auto r = Realm::get_shared_realm(config);

    r->update_schema({
        {"table",
         {{"int_set", PropertyType::Set | PropertyType::Int},
          {"decimal_set", PropertyType::Set | PropertyType::Decimal | PropertyType::Nullable},
          {"decimal_list", PropertyType::Array | PropertyType::Decimal | PropertyType::Nullable},
          {"link_set", PropertyType::Set | PropertyType::Object, "table2"}}},
        {"table2", {{"id", PropertyType::Int, Property::IsPrimary{true}}}},
        {"other_table",
         {{"int_set", PropertyType::Set | PropertyType::Int},
          {"link_set", PropertyType::Set | PropertyType::Object, "other_table2"}}},
        {"other_table2", {{"id", PropertyType::Int, Property::IsPrimary{true}}}},
    });

    auto& coordinator = *_impl::RealmCoordinator::get_coordinator(config.path);
    static_cast<void>(coordinator);

    auto table = r->read_group().get_table("class_table");
    auto table2 = r->read_group().get_table("class_table2");
    auto other_table = r->read_group().get_table("class_table");
    auto other_table2 = r->read_group().get_table("class_table2");

    ColKey col_int_set = table->get_column_key("int_set");
    ColKey col_decimal_set = table->get_column_key("decimal_set");
    ColKey col_decimal_list = table->get_column_key("decimal_list");

    ColKey col_link_set = table->get_column_key("link_set");
    ColKey col_link_obj_id = table2->get_column_key("id");
    ColKey other_col_link_set = table->get_column_key("link_set");

    auto write = [&](auto&& f) {
        r->begin_transaction();
        if constexpr (std::is_void_v<decltype(f())>) {
            f();
            r->commit_transaction();
            advance_and_notify(*r);
        }
        else {
            auto result = f();
            r->commit_transaction();
            advance_and_notify(*r);
            return result;
        }
    };

    auto obj = write([&] {
        return table->create_object();
    });

    auto other_obj = write([&] {
        return other_table->create_object();
    });

    SECTION("basics") {
        object_store::Set set{r, obj, col_int_set};

        write([&]() {
            CHECK(set.insert(123).second);
            CHECK(set.insert(456).second);
            CHECK(set.insert(0).second);
            CHECK(set.insert_any(-1).second);
            CHECK(!set.insert(456).second);
        });

        REQUIRE(set.is_valid());
        CHECK(set.size() == 4);
        CHECK(set.find(-1) == 0);
        CHECK(set.find(0) == 1);
        CHECK(set.get_any(2) == Mixed(123));
        CHECK(set.find_any(456) == 3);
        CHECK(set.find(999) == size_t(-1));

        write([&]() {
            CHECK(set.remove(123).second);
            CHECK(!set.remove(123).second);
            CHECK(set.remove_any(-1).second);
        });

        CHECK(set.size() == 2);

        write([&]() {
            obj.remove();
        });
        CHECK(!set.is_valid());
    }

    SECTION("nullable decimal") {
        object_store::Set set{r, obj, col_decimal_set};
        auto results = set.as_results();

        write([&]() {
            CHECK(set.insert(Decimal128(5)).second);
            CHECK(set.insert(Decimal128(realm::null())).second);
            CHECK(set.insert(Decimal128(7)).second);
        });

        REQUIRE(set.is_valid());
        CHECK(set.size() == 3);
        CHECK(results.index_of(Decimal128(realm::null())) == 0);
        auto sorted = results.sort({{"self", false}});
        CHECK(sorted.index_of(Decimal128(realm::null())) == 2);
    }

    SECTION("objects / links") {
        object_store::Set set{r, obj, col_link_set};

        Obj target1, target2, target3;
        write([&]() {
            target1 = table2->create_object_with_primary_key(123);
            target2 = table2->create_object_with_primary_key(456);
            target3 = table2->create_object_with_primary_key(789);
        });

        write([&]() {
            CHECK(set.insert(target1).second);
            CHECK(!set.insert(target1).second);
            CHECK(set.insert(target2).second);
            CHECK(set.insert(target3).second);
        });

        REQUIRE(set.is_valid());
        CHECK(set.size() == 3);

        CHECK(set.find(target1) != size_t(-1));
        CHECK(set.find(target2) != size_t(-1));
        CHECK(set.find(target3) != size_t(-1));

        write([&]() {
            target2.invalidate();
        });

        // Invalidating the object changes the reported size of the set.
        CHECK(set.size() == 2);

        CHECK_THROWS(set.find(target2));

        // Resurrect the tombstone of target2.
        write([&]() {
            target2 = table2->create_object_with_primary_key(456);
        });
        CHECK(set.find(target2));
        CHECK(set.size() == 3);
    }

    SECTION("max / min / sum / avg") {
        object_store::Set set{r, obj, col_int_set};

        write([&]() {
            CHECK(set.insert(123).second);
            CHECK(set.insert(456).second);
            CHECK(set.insert(0).second);
            CHECK(set.insert(-1).second);
        });

        auto x = set.min();

        REQUIRE(set.is_valid());
        CHECK(set.sum(col_int_set) == 578);
        CHECK(set.min(col_int_set) == -1);
        CHECK(set.max(col_int_set) == 456);
        CHECK(set.average(col_int_set) == 144.5);
    }

    SECTION("add_notification_block()") {
        CollectionChangeSet change;
        object_store::Set link_set{r, obj, col_link_set};
        object_store::Set int_set{r, obj, col_int_set};

        auto require_change = [&] {
            auto token = link_set.add_notification_callback([&](CollectionChangeSet c, std::exception_ptr) {
                change = c;
            });
            advance_and_notify(*r);
            return token;
        };

        auto require_no_change = [&] {
            bool first = true;
            auto token =
                link_set.add_notification_callback([&, first](CollectionChangeSet, std::exception_ptr) mutable {
                    REQUIRE(first);
                    first = false;
                });
            advance_and_notify(*r);
            return token;
        };

        SECTION("modifying the set sends change notifications") {
            Obj target1, target2, target3;
            write([&]() {
                target1 = table2->create_object_with_primary_key(123);
                target2 = table2->create_object_with_primary_key(456);
                target3 = table2->create_object_with_primary_key(789);
            });

            auto token = require_change();

            write([&]() {
                CHECK(link_set.insert(target1).second);
                CHECK(!link_set.insert(target1).second);
                CHECK(link_set.insert(target2).second);
                CHECK(link_set.insert(target3).second);
            });

            REQUIRE(link_set.size() == 3);

            write([&] {
                CHECK(link_set.remove(target2).second);
            });
            REQUIRE(link_set.size() == 2);
            REQUIRE_INDICES(change.deletions, 1);
        }

        SECTION("modifying a different set doesn't send a change notification") {
            auto token = require_no_change();
            write([&] {
                CHECK(int_set.insert(123).second);
            });
        }

        SECTION("deleting the set sends change notification") {
            auto token = require_change();

            Obj target1, target2, target3;
            write([&]() {
                target1 = table2->create_object_with_primary_key(123);
                target2 = table2->create_object_with_primary_key(456);
                target3 = table2->create_object_with_primary_key(789);
            });

            write([&]() {
                CHECK(link_set.insert(target1).second);
                CHECK(!link_set.insert(target1).second);
                CHECK(link_set.insert(target2).second);
                CHECK(link_set.insert(target3).second);
            });

            write([&] {
                link_set.remove_all();
            });
            REQUIRE_INDICES(change.deletions, 0, 1, 2);

            // Should not resend delete all notification after another commit
            change = {};
            write([&] {
                table->create_object();
            });
            REQUIRE(change.empty());
        }

        SECTION("deleting an empty set sends a change notification") {
            auto token = require_change();
            REQUIRE(link_set.size() == 0);
            REQUIRE(!change.collection_root_was_deleted);

            write([&]() {
                obj.remove();
            });
            REQUIRE(change.deletions.empty());
            REQUIRE(change.collection_root_was_deleted);

            // Should not resend delete all notification after another commit
            change = {};
            write([&] {
                table->create_object();
            });
            REQUIRE(change.empty());
        }
    }

    SECTION("find(Query)") {
        object_store::Set set{r, obj, col_link_set};

        Obj target1, target2, target3;
        write([&]() {
            target1 = table2->create_object_with_primary_key(123);
            target2 = table2->create_object_with_primary_key(456);
            target3 = table2->create_object_with_primary_key(789);
        });

        write([&]() {
            CHECK(set.insert(target1).second);
            CHECK(!set.insert(target1).second);
            CHECK(set.insert(target2).second);
            CHECK(set.insert(target3).second);
        });

        REQUIRE(set.is_valid());
        CHECK(set.size() == 3);

        SECTION("returns index in set for boxed Object") {
            REQUIRE(set.find(std::move(table2->where().equal(col_link_obj_id, 123))) == 0);
            REQUIRE(set.find(std::move(table2->where().equal(col_link_obj_id, 456))) == 1);
            REQUIRE(set.find(std::move(table2->where().equal(col_link_obj_id, 789))) == 2);
        }
    }

    SECTION("is_superset_of") {
        object_store::Set set{r, obj, col_link_set};
        object_store::Set set2{r, other_obj, other_col_link_set};

        std::vector<Obj> targets;
        write([&]() {
            targets.push_back(table2->create_object_with_primary_key(123));
            targets.push_back(table2->create_object_with_primary_key(456));
            targets.push_back(table2->create_object_with_primary_key(789));
            targets.push_back(table2->create_object_with_primary_key(321));
            targets.push_back(table2->create_object_with_primary_key(654));
            targets.push_back(table2->create_object_with_primary_key(987));
        });

        write([&]() {
            for (auto& obj : targets) {
                CHECK(set.insert(obj).second);
            }
            CHECK(set2.insert(targets[0]).second);
            CHECK(set2.insert(targets[1]).second);
            CHECK(set2.insert(targets[2]).second);
        });

        REQUIRE(set.is_valid());
        CHECK(set.size() == 6);
        REQUIRE(set2.is_valid());
        CHECK(set2.size() == 3);

        SECTION("set2 is a subset of set") {
            REQUIRE(set2.is_subset_of(set));
            REQUIRE_FALSE(set.is_subset_of(set2));
        }
    }

    SECTION("intersects") {
        object_store::Set set{r, obj, col_link_set};
        object_store::Set set2{r, other_obj, other_col_link_set};

        std::vector<Obj> targets;
        write([&]() {
            targets.push_back(table2->create_object_with_primary_key(123));
            targets.push_back(table2->create_object_with_primary_key(456));
            targets.push_back(table2->create_object_with_primary_key(789));
            targets.push_back(table2->create_object_with_primary_key(321));
            targets.push_back(table2->create_object_with_primary_key(654));
            targets.push_back(table2->create_object_with_primary_key(987));
        });

        std::vector<Obj> other_targets;
        write([&]() {
            other_targets.push_back(other_table2->create_object_with_primary_key(111));
            other_targets.push_back(other_table2->create_object_with_primary_key(222));
            other_targets.push_back(other_table2->create_object_with_primary_key(333));
        });

        write([&]() {
            for (auto& obj : targets) {
                CHECK(set.insert(obj).second);
            }
            for (auto& obj : other_targets) {
                CHECK(set2.insert(obj).second);
            }
            CHECK(set2.insert(targets[0]).second);
            CHECK(set2.insert(targets[1]).second);
            CHECK(set2.insert(targets[2]).second);
        });

        REQUIRE(set.is_valid());
        CHECK(set.size() == 6);
        REQUIRE(set2.is_valid());
        CHECK(set2.size() == 6);


        SECTION("set2 intersects set") {
            // (123, 456, 789, (321, 654, 987), 111, 222, 333)
            REQUIRE(set2.intersects(set));
            REQUIRE(set.intersects(set2));
            write([&]() {
                set2.remove(targets[0]);
                set2.remove(targets[1]);
                set2.remove(targets[2]);
            });
            // (123, 456, 789, (), 111, 222, 333)
            REQUIRE_FALSE(set2.intersects(set));
        }
    }

    SECTION("assign intersection") {
        object_store::Set set{r, obj, col_link_set};
        object_store::Set set2{r, other_obj, other_col_link_set};

        std::vector<Obj> targets;
        write([&]() {
            targets.push_back(table2->create_object_with_primary_key(123));
            targets.push_back(table2->create_object_with_primary_key(456));
            targets.push_back(table2->create_object_with_primary_key(789));
            targets.push_back(table2->create_object_with_primary_key(321));
            targets.push_back(table2->create_object_with_primary_key(654));
            targets.push_back(table2->create_object_with_primary_key(987));
        });

        std::vector<Obj> other_targets;
        write([&]() {
            other_targets.push_back(other_table2->create_object_with_primary_key(111));
            other_targets.push_back(other_table2->create_object_with_primary_key(222));
            other_targets.push_back(other_table2->create_object_with_primary_key(333));
        });

        write([&]() {
            for (auto& obj : targets) {
                CHECK(set.insert(obj).second);
            }
            for (auto& obj : other_targets) {
                CHECK(set2.insert(obj).second);
            }
            CHECK(set2.insert(targets[0]).second);
            CHECK(set2.insert(targets[1]).second);
            CHECK(set2.insert(targets[2]).second);
        });

        REQUIRE(set.is_valid());
        CHECK(set.size() == 6);
        REQUIRE(set2.is_valid());
        CHECK(set2.size() == 6);

        SECTION("set2 intersects set") {
            // (123, 456, 789, (321, 654, 987), 111, 222, 333)
            write([&] {
                set2.assign_intersection(set);
            });
            CHECK(set2.size() == 3);
        }
    }

    SECTION("assign union") {
        object_store::Set set{r, obj, col_link_set};
        object_store::Set set2{r, other_obj, other_col_link_set};

        std::vector<Obj> targets;
        write([&]() {
            targets.push_back(table2->create_object_with_primary_key(123));
            targets.push_back(table2->create_object_with_primary_key(456));
            targets.push_back(table2->create_object_with_primary_key(789));
            targets.push_back(table2->create_object_with_primary_key(321));
            targets.push_back(table2->create_object_with_primary_key(654));
            targets.push_back(table2->create_object_with_primary_key(987));
        });

        std::vector<Obj> other_targets;
        write([&]() {
            other_targets.push_back(other_table2->create_object_with_primary_key(111));
            other_targets.push_back(other_table2->create_object_with_primary_key(222));
            other_targets.push_back(other_table2->create_object_with_primary_key(333));
        });

        write([&]() {
            for (auto& obj : targets) {
                CHECK(set.insert(obj).second);
            }
            for (auto& obj : other_targets) {
                CHECK(set2.insert(obj).second);
            }
            CHECK(set2.insert(targets[0]).second);
            CHECK(set2.insert(targets[1]).second);
            CHECK(set2.insert(targets[2]).second);
        });

        REQUIRE(set.is_valid());
        CHECK(set.size() == 6);
        REQUIRE(set2.is_valid());
        CHECK(set2.size() == 6);

        SECTION("set2 intersects set") {
            // (123, 456, 789, (321, 654, 987), 111, 222, 333)
            write([&] {
                set2.assign_union(set);
            });
            CHECK(set2.size() == 9);
        }
    }

    SECTION("assign difference") {
        object_store::Set set{r, obj, col_link_set};
        object_store::Set set2{r, other_obj, other_col_link_set};

        std::vector<Obj> targets;
        write([&]() {
            targets.push_back(table2->create_object_with_primary_key(123));
            targets.push_back(table2->create_object_with_primary_key(456));
            targets.push_back(table2->create_object_with_primary_key(789));
            targets.push_back(table2->create_object_with_primary_key(321));
            targets.push_back(table2->create_object_with_primary_key(654));
            targets.push_back(table2->create_object_with_primary_key(987));
        });

        std::vector<Obj> other_targets;
        write([&]() {
            other_targets.push_back(other_table2->create_object_with_primary_key(111));
            other_targets.push_back(other_table2->create_object_with_primary_key(222));
            other_targets.push_back(other_table2->create_object_with_primary_key(333));
        });

        write([&]() {
            for (auto& obj : targets) {
                CHECK(set.insert(obj).second);
            }
            for (auto& obj : other_targets) {
                CHECK(set2.insert(obj).second);
            }
            CHECK(set2.insert(targets[0]).second);
            CHECK(set2.insert(targets[1]).second);
            CHECK(set2.insert(targets[2]).second);
        });

        REQUIRE(set.is_valid());
        CHECK(set.size() == 6);
        REQUIRE(set2.is_valid());
        CHECK(set2.size() == 6);

        SECTION("set2 intersects set") {
            // (123, 456, 789, (321, 654, 987), 111, 222, 333)
            write([&] {
                set2.assign_difference(set);
            });
            CHECK(set2.size() == 3);
        }
    }

    SECTION("set operations against list") {
        object_store::Set set{r, obj, col_decimal_set};
        List list{r, obj, col_decimal_list};

        write([&]() {
            CHECK(set.insert(Decimal128(5)).second);
            CHECK(set.insert(Decimal128(realm::null())).second);
            CHECK(set.insert(Decimal128(7)).second);
        });

        write([&]() {
            list.add(Decimal128(4));
            list.add(Decimal128(realm::null()));
            list.add(Decimal128(7));
            list.add(Decimal128(4));
        });
        REQUIRE(set.intersects(list));
        write([&]() {
            set.assign_union(list); // set == { null, 4, 5, 7 }
        });
        REQUIRE(set.size() == 4);
        REQUIRE(set.is_strict_superset_of(list));
        write([&]() {
            set.assign_difference(list); // set == { 5 }
        });
        REQUIRE(set.size() == 1);
        write([&]() {
            CHECK(set.insert(Decimal128(4)).second); // set == { 4, 5 }
            set.assign_symmetric_difference(list);   // set == { null, 5, 7 }
        });
        REQUIRE(set.size() == 3);
        write([&]() {
            set.assign_intersection(list); // set == { null, 7 }
        });
        REQUIRE(set.size() == 2);
        REQUIRE(set.is_strict_subset_of(list));
        write([&]() {
            CHECK(set.insert(Decimal128(4)).second); // set == { null, 4, 7 }
        });
        REQUIRE(set.set_equals(list));
    }
}


TEST_CASE("set with mixed links", "[set]") {
    InMemoryTestFile config;
    config.cache = false;
    config.automatic_change_notifications = false;
    config.schema = Schema{
        {"object", {{"value", PropertyType::Set | PropertyType::Mixed | PropertyType::Nullable}}},
        {"target1",
         {{"value1", PropertyType::Int}, {"link1", PropertyType::Object | PropertyType::Nullable, "target1"}}},
        {"target2",
         {{"value2", PropertyType::Int}, {"link2", PropertyType::Object | PropertyType::Nullable, "target2"}}}};

    auto r = Realm::get_shared_realm(config);

    auto table = r->read_group().get_table("class_object");
    auto target1 = r->read_group().get_table("class_target1");
    auto target2 = r->read_group().get_table("class_target2");
    ColKey col_value1 = target1->get_column_key("value1");
    ColKey col_value2 = target2->get_column_key("value2");
    ColKey col_link1 = target1->get_column_key("link1");
    r->begin_transaction();
    Obj obj = table->create_object();
    Obj obj1 = table->create_object(); // empty set
    Obj target1_obj = target1->create_object().set(col_value1, 100);
    Obj target2_obj = target2->create_object().set(col_value2, 200);
    ColKey col = table->get_column_key("value");

    object_store::Set set(r, obj, col);
    CppContext ctx(r);

    set.insert(Mixed{ObjLink(target1->get_key(), target1_obj.get_key())});
    set.insert(Mixed{ObjLink(target2->get_key(), target2_obj.get_key())});
    set.insert(Mixed{});
    set.insert(Mixed{int64_t{42}});
    r->commit_transaction();

    Results all_objects(r, table->where());
    REQUIRE(all_objects.size() == 2);
    CollectionChangeSet local_changes;
    auto x = all_objects.add_notification_callback([&local_changes](CollectionChangeSet c, std::exception_ptr) {
        local_changes = c;
    });
    advance_and_notify(*r);

    SECTION("insertion") {
        r->begin_transaction();
        table->create_object();
        r->commit_transaction();
        advance_and_notify(*r);
        REQUIRE(local_changes.insertions.count() == 1);
        REQUIRE(local_changes.modifications.count() == 0);
        REQUIRE(local_changes.deletions.count() == 0);
    }
    SECTION("insert to set is a modification") {
        r->begin_transaction();
        set.insert(Mixed{"hello"});
        r->commit_transaction();
        advance_and_notify(*r);
        REQUIRE(local_changes.insertions.count() == 0);
        REQUIRE(local_changes.modifications.count() == 1);
        REQUIRE(local_changes.deletions.count() == 0);
    }
    SECTION("modify a linked object is a modification") {
        r->begin_transaction();
        target1_obj.set(col_value1, 1000);
        r->commit_transaction();
        advance_and_notify(*r);
        REQUIRE(local_changes.insertions.count() == 0);
        REQUIRE(local_changes.modifications.count() == 1);
        REQUIRE(local_changes.deletions.count() == 0);
    }
    SECTION("modify a linked object once removed is a modification") {
        r->begin_transaction();
        auto target1_obj2 = target1->create_object().set(col_value1, 1000);
        target1_obj.set(col_link1, target1_obj2.get_key());
        r->commit_transaction();
        advance_and_notify(*r);
        local_changes = {};
        r->begin_transaction();
        target1_obj2.set(col_value1, 2000);
        r->commit_transaction();
        advance_and_notify(*r);
        REQUIRE(local_changes.insertions.count() == 0);
        REQUIRE(local_changes.modifications.count() == 1);
        REQUIRE(local_changes.deletions.count() == 0);
    }
}
