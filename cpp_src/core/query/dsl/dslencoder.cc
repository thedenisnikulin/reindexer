#include "dslencoder.h"
#include <unordered_map>
#include "core/cjson/jsonbuilder.h"
#include "core/keyvalue/key_string.h"
#include "core/keyvalue/p_string.h"
#include "core/query/query.h"
#include "core/queryresults/aggregationresult.h"
#include "dslparser.h"

using std::unordered_map;

struct EnumClassHash {
	template <typename T>
	size_t operator()(T t) const {
		return static_cast<size_t>(t);
	}
};

namespace reindexer {
namespace dsl {

const unordered_map<JoinType, string, EnumClassHash> join_types = {{InnerJoin, "inner"}, {LeftJoin, "left"}, {OrInnerJoin, "orinner"}};

const unordered_map<CondType, string, EnumClassHash> cond_map = {
	{CondAny, "any"},	  {CondEq, "eq"},	{CondLt, "lt"},			{CondLe, "le"},		  {CondGt, "gt"},	  {CondGe, "ge"},
	{CondRange, "range"}, {CondSet, "set"}, {CondAllSet, "allset"}, {CondEmpty, "empty"}, {CondLike, "like"}, {CondDWithin, "dwithin"},
};

const unordered_map<OpType, string, EnumClassHash> op_map = {{OpOr, "or"}, {OpAnd, "and"}, {OpNot, "not"}};

const unordered_map<CalcTotalMode, string, EnumClassHash> reqtotal_values = {
	{ModeNoTotal, "disabled"}, {ModeAccurateTotal, "enabled"}, {ModeCachedTotal, "cached"}};

template <typename T>
string get(unordered_map<T, string, EnumClassHash> const& m, const T& key) {
	auto it = m.find(key);
	if (it != m.end()) return it->second;
	assertrx(it != m.end());
	return string();
}

void encodeSorting(const SortingEntries& sortingEntries, JsonBuilder& builder) {
	auto arrNode = builder.Array("sort");

	for (const SortingEntry& sortingEntry : sortingEntries) {
		arrNode.Object().Put("field", sortingEntry.expression).Put("desc", sortingEntry.desc);
	}
}

void encodeSingleJoinQuery(const JoinedQuery& joinQuery, JsonBuilder& builder);

void encodeJoins(const Query& query, JsonBuilder& builder) {
	for (const auto& joinQuery : query.joinQueries_) {
		if (joinQuery.joinType == LeftJoin) {
			auto node = builder.Object();
			encodeSingleJoinQuery(joinQuery, node);
		}
	}
}

void encodeEqualPositions(const EqualPositions_t& equalPositions, JsonBuilder& builder) {
	if (equalPositions.empty()) return;
	auto node = builder.Object();
	auto epNodePositions = node.Array("equal_positions");
	for (const auto& eqPos : equalPositions) {
		auto epNodePosition = epNodePositions.Object(std::string_view());
		auto epNodePositionArr = epNodePosition.Array("positions");
		for (const auto& field : eqPos) epNodePositionArr.Put(nullptr, field);
	}
}

void encodeFilters(const Query& query, JsonBuilder& builder) {
	auto arrNode = builder.Array("filters");
	query.entries.ToDsl(query, arrNode);
	encodeJoins(query, arrNode);
	encodeEqualPositions(query.entries.equalPositions, arrNode);
}

void toDsl(const Query& query, JsonBuilder& builder);

void encodeMergedQueries(const Query& query, JsonBuilder& builder) {
	auto arrNode = builder.Array("merge_queries");

	for (const Query& mq : query.mergeQueries_) {
		auto node = arrNode.Object();
		toDsl(mq, node);
	}
}

void encodeSelectFilter(const Query& query, JsonBuilder& builder) {
	auto arrNode = builder.Array("select_filter");
	for (auto& str : query.selectFilter_) arrNode.Put(nullptr, str);
}

void encodeSelectFunctions(const Query& query, JsonBuilder& builder) {
	auto arrNode = builder.Array("select_functions");
	for (auto& str : query.selectFunctions_) arrNode.Put(nullptr, str);
}

void encodeAggregationFunctions(const Query& query, JsonBuilder& builder) {
	auto arrNode = builder.Array("aggregations");

	for (auto& entry : query.aggregations_) {
		auto aggNode = arrNode.Object();
		aggNode.Put("type", AggregationResult::aggTypeToStr(entry.type_));
		encodeSorting(entry.sortingEntries_, aggNode);
		if (entry.limit_ != UINT_MAX) aggNode.Put("limit", entry.limit_);
		if (entry.offset_ != 0) aggNode.Put("offset", entry.offset_);
		auto fldNode = aggNode.Array("fields");
		for (const auto& field : entry.fields_) {
			fldNode.Put(nullptr, field);
		}
	}
}

void encodeJoinEntry(const QueryJoinEntry& joinEntry, JsonBuilder& builder) {
	builder.Put("left_field", joinEntry.index_);
	builder.Put("right_field", joinEntry.joinIndex_);
	builder.Put("cond", get(cond_map, joinEntry.condition_));
	builder.Put("op", get(op_map, joinEntry.op_));
}

void encodeSingleJoinQuery(const JoinedQuery& joinQuery, JsonBuilder& builder) {
	using namespace std::string_view_literals;
	auto node = builder.Object("join_query"sv);

	node.Put("type", get(join_types, joinQuery.joinType));
	node.Put("namespace", joinQuery._namespace);
	node.Put("limit", joinQuery.count);
	node.Put("offset", joinQuery.start);

	encodeFilters(joinQuery, node);
	encodeSorting(joinQuery.sortingEntries_, node);

	auto arr1 = node.Array("on");

	for (auto& joinEntry : joinQuery.joinEntries_) {
		auto obj1 = arr1.Object();
		encodeJoinEntry(joinEntry, obj1);
	}
	arr1.End();	 // Close array

	auto selectFilters = node.Array("select_filter");
	for (const auto& str : joinQuery.selectFilter_) {
		selectFilters.Put(nullptr, str);
	}
}

void encodeFilter(const QueryEntry& qentry, JsonBuilder& builder) {
	if (qentry.distinct) return;
	builder.Put("cond", get(cond_map, CondType(qentry.condition)));
	builder.Put("field", qentry.index);

	if (qentry.values.empty()) return;
	if (qentry.values.size() > 1 || qentry.values[0].Type() == KeyValueTuple) {
		auto arrNode = builder.Array("value");
		for (const Variant& kv : qentry.values) {
			arrNode.Put(nullptr, kv);
		}
	} else {
		builder.Put("value", qentry.values[0]);
	}
}

void encodeDropFields(const Query& query, JsonBuilder& builder) {
	auto dropFields = builder.Array("drop_fields");
	for (const UpdateEntry& updateEntry : query.UpdateFields()) {
		if (updateEntry.mode == FieldModeDrop) {
			dropFields.Put(0, updateEntry.column);
		}
	}
}

void encodeUpdateFields(const Query& query, JsonBuilder& builder) {
	auto updateFields = builder.Array("update_fields");
	for (const UpdateEntry& updateEntry : query.UpdateFields()) {
		if (updateEntry.mode == FieldModeSet || updateEntry.mode == FieldModeSetJson) {
			bool isObject = (updateEntry.mode == FieldModeSetJson);
			auto field = updateFields.Object(0);
			if (isObject) {
				field.Put("type", "object");
			} else if (updateEntry.isExpression) {
				field.Put("type", "expression");
			} else {
				field.Put("type", "value");
			}
			field.Put("name", updateEntry.column);
			field.Put("is_array", updateEntry.values.IsArrayValue());
			auto values = field.Array("values");
			for (const Variant& v : updateEntry.values) {
				if (isObject) {
					values.Json(nullptr, p_string(v));
				} else {
					values.Put(0, v);
				}
			}
		}
	}
}

void toDsl(const Query& query, JsonBuilder& builder) {
	switch (query.Type()) {
		case QueryType::QuerySelect: {
			builder.Put("namespace", query._namespace);
			builder.Put("limit", query.count);
			builder.Put("offset", query.start);
			builder.Put("req_total", get(reqtotal_values, query.calcTotal));
			builder.Put("explain", query.explain_);
			builder.Put("type", "select");
			auto strictMode = strictModeToString(query.strictMode);
			if (!strictMode.empty()) {
				builder.Put("strict_mode", strictMode);
			}
			builder.Put("select_with_rank", query.IsWithRank());

			encodeSelectFilter(query, builder);
			encodeSelectFunctions(query, builder);
			encodeSorting(query.sortingEntries_, builder);
			encodeFilters(query, builder);
			encodeMergedQueries(query, builder);
			encodeAggregationFunctions(query, builder);
			break;
		}
		case QueryType::QueryUpdate: {
			builder.Put("namespace", query._namespace);
			builder.Put("explain", query.explain_);
			builder.Put("type", "update");
			encodeFilters(query, builder);
			bool withDropEntries = false, withUpdateEntries = false;
			for (const UpdateEntry& updateEntry : query.UpdateFields()) {
				if (updateEntry.mode == FieldModeDrop) {
					if (!withDropEntries) withDropEntries = true;
				}
				if (updateEntry.mode == FieldModeSet || updateEntry.mode == FieldModeSetJson) {
					if (!withUpdateEntries) withUpdateEntries = true;
				}
			}
			if (withDropEntries) {
				encodeDropFields(query, builder);
			}
			if (withUpdateEntries) {
				encodeUpdateFields(query, builder);
			}
			break;
		}
		case QueryType::QueryDelete: {
			builder.Put("namespace", query._namespace);
			builder.Put("explain", query.explain_);
			builder.Put("type", "delete");
			encodeFilters(query, builder);
			break;
		}
		case QueryType::QueryTruncate: {
			builder.Put("namespace", query._namespace);
			builder.Put("type", "truncate");
			break;
		}
		default:
			break;
	}
}

std::string toDsl(const Query& query) {
	WrSerializer ser;
	JsonBuilder builder(ser);
	toDsl(query, builder);

	builder.End();
	return string(ser.Slice());
}

}  // namespace dsl

void QueryEntries::toDsl(const_iterator it, const_iterator to, const Query& parentQuery, JsonBuilder& builder) {
	for (; it != to; ++it) {
		auto node = builder.Object();
		node.Put("op", dsl::get(dsl::op_map, it->operation));
		it->InvokeAppropriate<void>(
			Skip<AlwaysFalse>{},
			[&it, &node, &parentQuery](const QueryEntriesBracket& bracket) {
				auto arrNode = node.Array("filters");
				toDsl(it.cbegin(), it.cend(), parentQuery, arrNode);
				dsl::encodeEqualPositions(bracket.equalPositions, arrNode);
			},
			[&node](const QueryEntry& qe) {
				if (qe.distinct) return;
				dsl::encodeFilter(qe, node);
			},
			[&node, &parentQuery](const JoinQueryEntry& jqe) {
				assertrx(jqe.joinIndex < parentQuery.joinQueries_.size());
				dsl::encodeSingleJoinQuery(parentQuery.joinQueries_[jqe.joinIndex], node);
			},
			[&node](const BetweenFieldsQueryEntry& qe) {
				node.Put("cond", dsl::get(dsl::cond_map, CondType(qe.Condition())));
				node.Put("first_field", qe.firstIndex);
				node.Put("second_field", qe.secondIndex);
			});
	}
}

}  // namespace reindexer
