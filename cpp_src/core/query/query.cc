
#include "core/query/query.h"
#include "core/query/dsl/dslencoder.h"
#include "core/query/dsl/dslparser.h"
#include "core/query/sql/sqlencoder.h"
#include "core/query/sql/sqlparser.h"
#include "tools/serializer.h"

namespace reindexer {

using namespace std::string_view_literals;

const std::string_view kLsnIndexName = "#lsn"sv;
const std::string_view kSlaveVersionIndexName = "#slave_version"sv;

Query::Query(const string &__namespace, unsigned _start, unsigned _count, CalcTotalMode _calcTotal)
	: _namespace(__namespace), start(_start), count(_count), calcTotal(_calcTotal) {}

bool Query::operator==(const Query &obj) const {
	if (entries != obj.entries) return false;
	if (aggregations_ != obj.aggregations_) return false;

	if (_namespace != obj._namespace) return false;
	if (sortingEntries_ != obj.sortingEntries_) return false;
	if (calcTotal != obj.calcTotal) return false;
	if (start != obj.start) return false;
	if (count != obj.count) return false;
	if (debugLevel != obj.debugLevel) return false;
	if (strictMode != obj.strictMode) return false;
	if (forcedSortOrder_.size() != obj.forcedSortOrder_.size()) return false;
	for (size_t i = 0, s = forcedSortOrder_.size(); i < s; ++i) {
		if (forcedSortOrder_[i].RelaxCompare(obj.forcedSortOrder_[i]) != 0) return false;
	}

	if (selectFilter_ != obj.selectFilter_) return false;
	if (selectFunctions_ != obj.selectFunctions_) return false;
	if (joinQueries_ != obj.joinQueries_) return false;
	if (mergeQueries_ != obj.mergeQueries_) return false;
	if (updateFields_ != obj.updateFields_) return false;

	return true;
}

bool JoinedQuery::operator==(const JoinedQuery &obj) const {
	if (joinEntries_ != obj.joinEntries_) return false;
	if (joinType != obj.joinType) return false;
	return Query::operator==(obj);
}
void Query::FromSQL(std::string_view q) { SQLParser(*this).Parse(q); }

Error Query::FromJSON(const string &dsl) { return dsl::Parse(dsl, *this); }

string Query::GetJSON() const { return dsl::toDsl(*this); }

Query &Query::SetObject(std::string field, VariantArray value, bool hasExpressions) & {
	for (auto &it : value) {
		if (it.Type() != KeyValueString) {
			throw Error(errLogic, "Unexpected variant type in SetObject: %s. Expecting KeyValueString with JSON-content",
						Variant::TypeName(it.Type()));
		}
	}
	updateFields_.emplace_back(std::move(field), std::move(value), FieldModeSetJson, hasExpressions);
	return *this;
}

WrSerializer &Query::GetSQL(WrSerializer &ser, bool stripArgs) const { return SQLEncoder(*this).GetSQL(ser, stripArgs); }

string Query::GetSQL(bool stripArgs) const {
	WrSerializer ser;
	return string(GetSQL(ser, stripArgs).Slice());
}

void Query::deserialize(Serializer &ser, bool &hasJoinConditions) {
	bool end = false;
	std::vector<std::pair<size_t, EqualPosition_t>> equalPositions;
	while (!end && !ser.Eof()) {
		int qtype = ser.GetVarUint();
		switch (qtype) {
			case QueryCondition: {
				QueryEntry qe;
				qe.index = string(ser.GetVString());
				OpType op = OpType(ser.GetVarUint());
				qe.condition = CondType(ser.GetVarUint());
				int cnt = ser.GetVarUint();
				if (qe.condition == CondDWithin) {
					if (cnt != 3) {
						throw Error(errParseBin, "Expected point and distance for DWithin");
					}
					VariantArray point;
					point.reserve(2);
					point.emplace_back(ser.GetVariant().EnsureHold());
					point.emplace_back(ser.GetVariant().EnsureHold());
					qe.values.reserve(2);
					qe.values.emplace_back(std::move(point));
					qe.values.emplace_back(ser.GetVariant().EnsureHold());
				} else {
					qe.values.reserve(cnt);
					while (cnt--) qe.values.emplace_back(ser.GetVariant().EnsureHold());
				}
				entries.Append(op, std::move(qe));
				break;
			}
			case QueryBetweenFieldsCondition: {
				OpType op = OpType(ser.GetVarUint());
				std::string firstField{ser.GetVString()};
				CondType condition = static_cast<CondType>(ser.GetVarUint());
				std::string secondField{ser.GetVString()};
				entries.Append(op, BetweenFieldsQueryEntry{std::move(firstField), condition, std::move(secondField)});
				break;
			}
			case QueryAlwaysFalseCondition: {
				const OpType op = OpType(ser.GetVarUint());
				entries.Append(op, AlwaysFalse{});
				break;
			}
			case QueryJoinCondition: {
				uint64_t type = ser.GetVarUint();
				assertrx(type != JoinType::LeftJoin);
				JoinQueryEntry joinEntry(ser.GetVarUint());
				hasJoinConditions = true;
				entries.Append((type == JoinType::OrInnerJoin) ? OpOr : OpAnd, std::move(joinEntry));
				break;
			}
			case QueryAggregation: {
				AggregateEntry ae;
				ae.type_ = static_cast<AggType>(ser.GetVarUint());
				size_t fieldsCount = ser.GetVarUint();
				ae.fields_.reserve(fieldsCount);
				while (fieldsCount--) ae.fields_.push_back(string(ser.GetVString()));
				auto pos = ser.Pos();
				bool aggEnd = false;
				while (!ser.Eof() && !aggEnd) {
					int atype = ser.GetVarUint();
					switch (atype) {
						case QueryAggregationSort: {
							auto fieldName = ser.GetVString();
							ae.sortingEntries_.push_back({string(fieldName), ser.GetVarUint() != 0});
							break;
						}
						case QueryAggregationLimit:
							ae.limit_ = ser.GetVarUint();
							break;
						case QueryAggregationOffset:
							ae.offset_ = ser.GetVarUint();
							break;
						default:
							ser.SetPos(pos);
							aggEnd = true;
					}
					pos = ser.Pos();
				}
				aggregations_.push_back(std::move(ae));
				break;
			}
			case QueryDistinct: {
				QueryEntry qe;
				qe.index = string(ser.GetVString());
				if (!qe.index.empty()) {
					qe.distinct = true;
					qe.condition = CondAny;
					entries.Append(OpAnd, std::move(qe));
				}
				break;
			}
			case QuerySortIndex: {
				SortingEntry sortingEntry;
				sortingEntry.expression = string(ser.GetVString());
				sortingEntry.desc = bool(ser.GetVarUint());
				if (sortingEntry.expression.length()) {
					sortingEntries_.push_back(std::move(sortingEntry));
				}
				int cnt = ser.GetVarUint();
				if (cnt != 0 && sortingEntries_.size() != 1) {
					throw Error(errParams, "Forced sort order is allowed for the first sorting entry only");
				}
				forcedSortOrder_.reserve(cnt);
				while (cnt--) forcedSortOrder_.push_back(ser.GetVariant().EnsureHold());
				break;
			}
			case QueryJoinOn: {
				QueryJoinEntry qje;
				qje.op_ = OpType(ser.GetVarUint());
				qje.condition_ = CondType(ser.GetVarUint());
				qje.index_ = string(ser.GetVString());
				qje.joinIndex_ = string(ser.GetVString());
				reinterpret_cast<JoinedQuery *>(this)->joinEntries_.push_back(std::move(qje));
				break;
			}
			case QueryDebugLevel:
				debugLevel = ser.GetVarUint();
				break;
			case QueryStrictMode:
				strictMode = StrictMode(ser.GetVarUint());
				break;
			case QueryLimit:
				count = ser.GetVarUint();
				break;
			case QueryOffset:
				start = ser.GetVarUint();
				break;
			case QueryReqTotal:
				calcTotal = CalcTotalMode(ser.GetVarUint());
				break;
			case QuerySelectFilter:
				selectFilter_.push_back(string(ser.GetVString()));
				break;
			case QueryEqualPosition: {
				const unsigned bracketPosition = ser.GetVarUint();
				const unsigned fieldsCount = ser.GetVarUint();
				equalPositions.emplace_back(bracketPosition, fieldsCount);
				for (auto &field : equalPositions.back().second) field = ser.GetVString();
				break;
			}
			case QueryExplain:
				explain_ = true;
				break;
			case QueryWithRank:
				withRank_ = true;
				break;
			case QuerySelectFunction:
				selectFunctions_.push_back(string(ser.GetVString()));
				break;
			case QueryDropField: {
				Drop(string(ser.GetVString()));
				break;
			}
			case QueryUpdateFieldV2: {
				VariantArray val;
				string field(ser.GetVString());
				bool isArray = ser.GetVarUint();
				int numValues = ser.GetVarUint();
				bool hasExpressions = false;
				while (numValues--) {
					hasExpressions = ser.GetVarUint();
					val.emplace_back(ser.GetVariant().EnsureHold());
				}
				if (isArray) val.MarkArray();
				Set(std::move(field), std::move(val), hasExpressions);
				break;
			}
			case QueryUpdateField: {
				VariantArray val;
				string field(ser.GetVString());
				int numValues = ser.GetVarUint();
				bool isArray = numValues > 1;
				bool hasExpressions = false;
				while (numValues--) {
					hasExpressions = ser.GetVarUint();
					val.emplace_back(ser.GetVariant().EnsureHold());
				}
				if (isArray) val.MarkArray();
				Set(std::move(field), std::move(val), hasExpressions);
				break;
			}
			case QueryUpdateObject: {
				VariantArray val;
				string field(ser.GetVString());
				bool hasExpressions = false;
				int numValues = ser.GetVarUint();
				if (ser.GetVarUint() == 1) val.MarkArray();
				while (numValues--) {
					hasExpressions = ser.GetVarUint();
					val.emplace_back(ser.GetVariant().EnsureHold());
				}
				SetObject(std::move(field), std::move(val), hasExpressions);
				break;
			}
			case QueryOpenBracket: {
				OpType op = OpType(ser.GetVarUint());
				entries.OpenBracket(op);
				break;
			}
			case QueryCloseBracket:
				entries.CloseBracket();
				break;
			case QueryEnd:
				end = true;
				break;
			default:
				throw Error(errParseBin, "Unknown type %d while parsing binary buffer", qtype);
		}
	}
	for (const auto &eqPos : equalPositions) {
		if (eqPos.first == 0) {
			entries.equalPositions.emplace_back(std::move(eqPos.second));
		} else {
			entries.Get<QueryEntriesBracket>(eqPos.first - 1).equalPositions.emplace_back(std::move(eqPos.second));
		}
	}
	return;
}

void Query::Serialize(WrSerializer &ser, uint8_t mode) const {
	ser.PutVString(_namespace);
	entries.Serialize(ser);

	for (const auto &agg : aggregations_) {
		ser.PutVarUint(QueryAggregation);
		ser.PutVarUint(agg.type_);
		ser.PutVarUint(agg.fields_.size());
		for (const auto &field : agg.fields_) {
			ser.PutVString(field);
		}
		for (const auto &se : agg.sortingEntries_) {
			ser.PutVarUint(QueryAggregationSort);
			ser.PutVString(se.expression);
			ser.PutVarUint(se.desc);
		}
		if (agg.limit_ != UINT_MAX) {
			ser.PutVarUint(QueryAggregationLimit);
			ser.PutVarUint(agg.limit_);
		}
		if (agg.offset_ != 0) {
			ser.PutVarUint(QueryAggregationOffset);
			ser.PutVarUint(agg.offset_);
		}
	}

	for (const auto &sortginEntry : sortingEntries_) {
		ser.PutVarUint(QuerySortIndex);
		ser.PutVString(sortginEntry.expression);
		ser.PutVarUint(sortginEntry.desc);
		int cnt = forcedSortOrder_.size();
		ser.PutVarUint(cnt);
		for (auto &kv : forcedSortOrder_) ser.PutVariant(kv);
	}

	if (mode & WithJoinEntries) {
		for (const auto &qje : reinterpret_cast<const JoinedQuery *>(this)->joinEntries_) {
			ser.PutVarUint(QueryJoinOn);
			ser.PutVarUint(qje.op_);
			ser.PutVarUint(qje.condition_);
			ser.PutVString(qje.index_);
			ser.PutVString(qje.joinIndex_);
		}
	}

	for (const auto &equalPoses : entries.equalPositions) {
		ser.PutVarUint(QueryEqualPosition);
		ser.PutVarUint(0);
		ser.PutVarUint(equalPoses.size());
		for (const auto &ep : equalPoses) ser.PutVString(ep);
	}
	for (size_t i = 0; i < entries.Size(); ++i) {
		if (entries.IsSubTree(i)) {
			const auto &bracket = entries.Get<QueryEntriesBracket>(i);
			for (const auto &equalPoses : bracket.equalPositions) {
				ser.PutVarUint(QueryEqualPosition);
				ser.PutVarUint(i + 1);
				ser.PutVarUint(equalPoses.size());
				for (const auto &ep : equalPoses) ser.PutVString(ep);
			}
		}
	}

	ser.PutVarUint(QueryDebugLevel);
	ser.PutVarUint(debugLevel);

	if (strictMode != StrictModeNotSet) {
		ser.PutVarUint(QueryStrictMode);
		ser.PutVarUint(int(strictMode));
	}

	if (!(mode & SkipLimitOffset)) {
		if (HasLimit()) {
			ser.PutVarUint(QueryLimit);
			ser.PutVarUint(count);
		}
		if (HasOffset()) {
			ser.PutVarUint(QueryOffset);
			ser.PutVarUint(start);
		}
	}

	if (calcTotal != ModeNoTotal) {
		ser.PutVarUint(QueryReqTotal);
		ser.PutVarUint(calcTotal);
	}

	for (const auto &sf : selectFilter_) {
		ser.PutVarUint(QuerySelectFilter);
		ser.PutVString(sf);
	}

	if (explain_) {
		ser.PutVarUint(QueryExplain);
	}

	if (withRank_) {
		ser.PutVarUint(QueryWithRank);
	}

	for (const auto &field : updateFields_) {
		if (field.mode == FieldModeSet) {
			ser.PutVarUint(QueryUpdateFieldV2);
			ser.PutVString(field.column);
			ser.PutVarUint(field.values.IsArrayValue());
			ser.PutVarUint(field.values.size());
			for (const Variant &val : field.values) {
				ser.PutVarUint(field.isExpression);
				ser.PutVariant(val);
			}
		} else if (field.mode == FieldModeDrop) {
			ser.PutVarUint(QueryDropField);
			ser.PutVString(field.column);
		} else {
			throw Error(errLogic, "Unsupported item modification mode = %d", field.mode);
		}
	}

	ser.PutVarUint(QueryEnd);  // finita la commedia... of root query

	if (!(mode & SkipJoinQueries)) {
		for (const auto &jq : joinQueries_) {
			ser.PutVarUint(static_cast<int>(jq.joinType));
			jq.Serialize(ser, WithJoinEntries);
		}
	}

	if (!(mode & SkipMergeQueries)) {
		for (const auto &mq : mergeQueries_) {
			ser.PutVarUint(static_cast<int>(mq.joinType));
			mq.Serialize(ser, mode | WithJoinEntries);
		}
	}
}

void Query::Deserialize(Serializer &ser) {
	_namespace = string(ser.GetVString());
	bool hasJoinConditions = false;
	deserialize(ser, hasJoinConditions);

	bool nested = false;
	while (!ser.Eof()) {
		auto joinType = JoinType(ser.GetVarUint());
		JoinedQuery q1(string(ser.GetVString()));
		q1.joinType = joinType;
		q1.deserialize(ser, hasJoinConditions);
		q1.debugLevel = debugLevel;
		q1.strictMode = strictMode;
		if (joinType == JoinType::Merge) {
			mergeQueries_.emplace_back(std::move(q1));
			nested = true;
		} else {
			Query &q = nested ? mergeQueries_.back() : *this;
			if (joinType != JoinType::LeftJoin && !hasJoinConditions) {
				const size_t joinIdx = joinQueries_.size();
				entries.Append((joinType == JoinType::OrInnerJoin) ? OpOr : OpAnd, JoinQueryEntry{joinIdx});
			}
			q.joinQueries_.emplace_back(std::move(q1));
		}
	}
}

Query &Query::Join(JoinType joinType, const string &index, const string &joinIndex, CondType cond, OpType op, Query &&qr) & {
	QueryJoinEntry joinEntry;
	joinEntry.op_ = op;
	joinEntry.condition_ = cond;
	joinEntry.index_ = index;
	joinEntry.joinIndex_ = joinIndex;
	joinQueries_.emplace_back(joinType, std::move(qr));
	joinQueries_.back().joinEntries_.emplace_back(std::move(joinEntry));
	if (joinType != JoinType::LeftJoin) {
		entries.Append((joinType == JoinType::InnerJoin) ? OpType::OpAnd : OpType::OpOr, JoinQueryEntry(joinQueries_.size() - 1));
	}
	return *this;
}

Query &Query::Join(JoinType joinType, const string &index, const string &joinIndex, CondType cond, OpType op, const Query &qr) & {
	QueryJoinEntry joinEntry;
	joinEntry.op_ = op;
	joinEntry.condition_ = cond;
	joinEntry.index_ = index;
	joinEntry.joinIndex_ = joinIndex;
	joinQueries_.emplace_back(joinType, qr);
	joinQueries_.back().joinEntries_.emplace_back(std::move(joinEntry));
	if (joinType != JoinType::LeftJoin) {
		entries.Append((joinType == JoinType::InnerJoin) ? OpType::OpAnd : OpType::OpOr, JoinQueryEntry(joinQueries_.size() - 1));
	}
	return *this;
}

Query::OnHelper Query::Join(JoinType joinType, Query &&q) & {
	joinQueries_.emplace_back(joinType, std::move(q));
	if (joinType != JoinType::LeftJoin) {
		entries.Append((joinType == JoinType::InnerJoin) ? OpType::OpAnd : OpType::OpOr, JoinQueryEntry(joinQueries_.size() - 1));
	}
	return {*this, joinQueries_.back()};
}

Query::OnHelper Query::Join(JoinType joinType, const Query &q) & {
	joinQueries_.emplace_back(joinType, q);
	if (joinType != JoinType::LeftJoin) {
		entries.Append((joinType == JoinType::InnerJoin) ? OpType::OpAnd : OpType::OpOr, JoinQueryEntry(joinQueries_.size() - 1));
	}
	return {*this, joinQueries_.back()};
}

Query::OnHelperR Query::Join(JoinType joinType, Query &&q) && {
	joinQueries_.emplace_back(joinType, std::move(q));
	if (joinType != JoinType::LeftJoin) {
		entries.Append((joinType == JoinType::InnerJoin) ? OpType::OpAnd : OpType::OpOr, JoinQueryEntry(joinQueries_.size() - 1));
	}
	return {std::move(*this), joinQueries_.back()};
}

Query::OnHelperR Query::Join(JoinType joinType, const Query &q) && {
	joinQueries_.emplace_back(joinType, q);
	if (joinType != JoinType::LeftJoin) {
		entries.Append((joinType == JoinType::InnerJoin) ? OpType::OpAnd : OpType::OpOr, JoinQueryEntry(joinQueries_.size() - 1));
	}
	return {std::move(*this), joinQueries_.back()};
}

void Query::WalkNested(bool withSelf, bool withMerged, std::function<void(const Query &q)> visitor) const {
	if (withSelf) visitor(*this);
	if (withMerged)
		for (auto &mq : mergeQueries_) visitor(mq);
	for (auto &jq : joinQueries_) visitor(jq);
	for (auto &mq : mergeQueries_)
		for (auto &jq : mq.joinQueries_) visitor(jq);
}

bool Query::IsWALQuery() const noexcept {
	if (entries.Size() == 1 && entries.HoldsOrReferTo<QueryEntry>(0) && kLsnIndexName == entries.Get<QueryEntry>(0).index) {
		return true;
	} else if (entries.Size() == 2 && entries.HoldsOrReferTo<QueryEntry>(0) && entries.HoldsOrReferTo<QueryEntry>(1)) {
		const auto &index0 = entries.Get<QueryEntry>(0).index;
		const auto &index1 = entries.Get<QueryEntry>(1).index;
		return (kLsnIndexName == index0 && kSlaveVersionIndexName == index1) ||
			   (kLsnIndexName == index1 && kSlaveVersionIndexName == index0);
	}
	return false;
}

}  // namespace reindexer
