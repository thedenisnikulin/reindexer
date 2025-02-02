#include "joinedselector.h"
#include "core/namespace/namespaceimpl.h"
#include "core/queryresults/joinresults.h"
#include "nsselecter.h"

constexpr size_t kMaxIterationsScaleForInnerJoinOptimization = 100;

namespace reindexer {

void JoinedSelector::selectFromRightNs(QueryResults &joinItemR, const Query &query, bool &found, bool &matchedAtLeastOnce) {
	assertrx(rightNs_);

	JoinCacheRes joinResLong;
	joinResLong.key.SetData(joinQuery_, query);
	rightNs_->getFromJoinCache(joinResLong);

	rightNs_->getIndsideFromJoinCache(joinRes_);
	if (joinRes_.needPut) {
		rightNs_->putToJoinCache(joinRes_, preResult_);
	}
	if (joinResLong.haveData) {
		found = joinResLong.it.val.ids_->size();
		matchedAtLeastOnce = joinResLong.it.val.matchedAtLeastOnce;
		rightNs_->FillResult(joinItemR, joinResLong.it.val.ids_);
	} else {
		SelectCtx ctx(query, nullptr);
		ctx.preResult = preResult_;
		ctx.matchedAtLeastOnce = false;
		ctx.reqMatchedOnceFlag = true;
		ctx.skipIndexesLookup = true;
		ctx.functions = &selectFunctions_;
		rightNs_->Select(joinItemR, ctx, rdxCtx_);
		if (query.explain_) {
			preResult_->explainOneSelect = joinItemR.explainResults;
		}

		found = joinItemR.Count();
		matchedAtLeastOnce = ctx.matchedAtLeastOnce;
	}
	if (joinResLong.needPut) {
		JoinCacheVal val;
		val.ids_ = make_intrusive<intrusive_atomic_rc_wrapper<IdSet>>();
		val.matchedAtLeastOnce = matchedAtLeastOnce;
		for (auto &r : joinItemR.Items()) {
			val.ids_->Add(r.Id(), IdSet::Unordered, 0);
		}
		rightNs_->putToJoinCache(joinResLong, val);
	}
}

void JoinedSelector::selectFromPreResultValues(QueryResults &joinItemR, const Query &query, bool &found, bool &matchedAtLeastOnce) const {
	size_t matched = 0;
	for (const ItemRef &item : preResult_->values) {
		assertrx(!item.Value().IsFree());
		if (query.entries.CheckIfSatisfyConditions({preResult_->values.payloadType, item.Value()}, preResult_->values.tagsMatcher)) {
			if (++matched > query.count) break;
			found = true;
			joinItemR.Add(item);
		}
	}
	matchedAtLeastOnce = matched;
}

bool JoinedSelector::Process(IdType rowId, int nsId, ConstPayload payload, bool match) {
	++called_;
	if (optimized_ && !match) {
		matched_++;
		return true;
	}

	const auto startTime = ExplainCalc::Clock::now();
	// Put values to join conditions
	size_t i = 0;
	if (itemQuery_.explain_ && !preResult_->explainOneSelect.empty()) itemQuery_.explain_ = false;
	std::unique_ptr<Query> itemQueryCopy;
	Query *itemQueryPtr = &itemQuery_;
	for (auto &je : joinQuery_.joinEntries_) {
		const bool nonIndexedField = (je.idxNo == IndexValueType::SetByJsonPath);
		if (nonIndexedField) {
			VariantArray &values = itemQueryPtr->entries.Get<QueryEntry>(i).values;
			KeyValueType type = values.empty() ? KeyValueUndefined : values[0].Type();
			payload.GetByJsonPath(je.index_, leftNs_->tagsMatcher_, values, type);
		} else {
			const auto &index = *leftNs_->indexes_[je.idxNo];
			const auto &fields = index.Fields();
			if (fields.getJsonPathsLength() == 0) {
				payload.Get(fields[0], itemQueryPtr->entries.Get<QueryEntry>(i).values);
			} else {
				payload.GetByJsonPath(fields.getTagsPath(0), itemQueryPtr->entries.Get<QueryEntry>(i).values, index.KeyType());
			}
		}
		if (itemQueryPtr->entries.Get<QueryEntry>(i).values.empty()) {
			if (itemQueryPtr == &itemQuery_) {
				itemQueryCopy = std::unique_ptr<Query>{new Query(itemQuery_)};
				itemQueryPtr = itemQueryCopy.get();
			}
			itemQueryPtr->entries.SetValue(i, AlwaysFalse{});
		}
		++i;
	}
	itemQueryPtr->Limit(match ? joinQuery_.count : 0);

	bool found = false;
	bool matchedAtLeastOnce = false;
	QueryResults joinItemR;
	if (preResult_->dataMode == JoinPreResult::ModeValues) {
		selectFromPreResultValues(joinItemR, *itemQueryPtr, found, matchedAtLeastOnce);
	} else {
		selectFromRightNs(joinItemR, *itemQueryPtr, found, matchedAtLeastOnce);
	}
	if (match && found) {
		if (nsId >= static_cast<int>(result_.joined_.size())) {
			result_.joined_.resize(nsId + 1);
		}
		joins::NamespaceResults &nsJoinRes = result_.joined_[nsId];
		nsJoinRes.SetJoinedSelectorsCount(joinedSelectorsCount_);
		nsJoinRes.Insert(rowId, joinedFieldIdx_, std::move(joinItemR));
	}
	if (matchedAtLeastOnce) ++matched_;
	preResult_->selectTime += (ExplainCalc::Clock::now() - startTime);
	return matchedAtLeastOnce;
}

template <bool byJsonPath>
void JoinedSelector::readValuesFromRightNs(VariantArray &values, const Index &leftIndex, [[maybe_unused]] int rightIdxNo,
										   const std::string &rightIndex) const {
	const KeyValueType leftIndexType = leftIndex.SelectKeyType();
	for (IdType rowId : preResult_->ids) {
		if (rightNs_->items_[rowId].IsFree()) continue;
		const ConstPayload pl{rightNs_->payloadType_, rightNs_->items_[rowId]};
		VariantArray buffer;
		if constexpr (byJsonPath) {
			pl.GetByJsonPath(rightIndex, rightNs_->tagsMatcher_, buffer, leftIndexType);
		} else {
			pl.Get(rightIdxNo, buffer);
		}
		for (Variant &v : buffer) values.push_back(v.convert(leftIndexType));
	}
}

template <bool byJsonPath>
void JoinedSelector::readValuesFromPreResult(VariantArray &values, const Index &leftIndex, [[maybe_unused]] int rightIdxNo,
											 const std::string &rightIndex) const {
	const KeyValueType leftIndexType = leftIndex.SelectKeyType();
	for (const ItemRef &item : preResult_->values) {
		assertrx(!item.Value().IsFree());
		const ConstPayload pl{preResult_->values.payloadType, item.Value()};
		VariantArray buffer;
		if constexpr (byJsonPath) {
			pl.GetByJsonPath(rightIndex, preResult_->values.tagsMatcher, buffer, leftIndexType);
		} else {
			pl.Get(rightIdxNo, buffer);
		}
		for (Variant &v : buffer) values.push_back(v.convert(leftIndexType));
	}
}

void JoinedSelector::AppendSelectIteratorOfJoinIndexData(SelectIteratorContainer &iterators, int *maxIterations, unsigned sortId,
														 SelectFunction::Ptr selectFnc, const RdxContext &rdxCtx) {
	if (joinType_ != JoinType::InnerJoin || preResult_->executionMode != JoinPreResult::ModeExecute ||
		preResult_->dataMode == JoinPreResult::ModeIterators ||
		(preResult_->dataMode == JoinPreResult::ModeIdSet ? preResult_->ids.size() : preResult_->values.size()) >
			*maxIterations * kMaxIterationsScaleForInnerJoinOptimization) {
		return;
	}
	unsigned optimized = 0;
	assertrx(preResult_->dataMode != JoinPreResult::ModeValues || itemQuery_.entries.Size() == joinQuery_.joinEntries_.size());
	for (size_t i = 0; i < joinQuery_.joinEntries_.size(); ++i) {
		const QueryJoinEntry &joinEntry = joinQuery_.joinEntries_[i];
		if (joinEntry.op_ != OpAnd || (joinEntry.condition_ != CondEq && joinEntry.condition_ != CondSet) ||
			(i + 1 < joinQuery_.joinEntries_.size() && joinQuery_.joinEntries_[i + 1].op_ == OpOr) ||
			joinEntry.idxNo == IndexValueType::SetByJsonPath) {
			continue;
		}
		const auto &leftIndex = leftNs_->indexes_[joinEntry.idxNo];
		assertrx(!IsFullText(leftIndex->Type()));
		if (leftIndex->Opts().IsSparse()) continue;

		VariantArray values;
		if (preResult_->dataMode == JoinPreResult::ModeIdSet) {
			values.reserve(preResult_->ids.size());
			int rightIdxNo = IndexValueType::NotSet;
			if (rightNs_->getIndexByName(joinEntry.joinIndex_, rightIdxNo) && !rightNs_->indexes_[rightIdxNo]->Opts().IsSparse()) {
				readValuesFromRightNs<false>(values, *leftIndex, rightIdxNo, joinEntry.joinIndex_);
			} else {
				readValuesFromRightNs<true>(values, *leftIndex, rightIdxNo, joinEntry.joinIndex_);
			}
		} else {
			values.reserve(preResult_->values.size());
			assertrx(itemQuery_.entries.HoldsOrReferTo<QueryEntry>(i));
			const QueryEntry &qe = itemQuery_.entries.Get<QueryEntry>(i);
			assertrx(qe.index == joinEntry.joinIndex_);
			const int rightIdxNo = qe.idxNo;
			if (rightIdxNo == IndexValueType::SetByJsonPath) {
				readValuesFromPreResult<true>(values, *leftIndex, rightIdxNo, joinEntry.joinIndex_);
			} else {
				readValuesFromPreResult<false>(values, *leftIndex, rightIdxNo, joinEntry.joinIndex_);
			}
		}
		auto ctx = selectFnc ? selectFnc->CreateCtx(joinEntry.idxNo) : BaseFunctionCtx::Ptr{};
		assertrx(!ctx || ctx->type != BaseFunctionCtx::kFtCtx);

		if (leftIndex->Opts().GetCollateMode() == CollateUTF8) {
			for (auto &key : values) key.EnsureUTF8();
		}
		bool was = false;
		Index::SelectOpts opts;
		opts.maxIterations = iterators.GetMaxIterations();
		opts.indexesNotOptimized = !leftNs_->SortOrdersBuilt();
		opts.inTransaction = inTransaction_;

		for (SelectKeyResult &res : leftIndex->SelectKey(values, CondSet, sortId, opts, ctx, rdxCtx)) {
			if (!res.comparators_.empty()) continue;
			SelectIterator selIter{res, false, joinEntry.index_, false};
			selIter.Bind(leftNs_->payloadType_, joinEntry.idxNo);
			const int curIterations = selIter.GetMaxIterations();
			if (curIterations && curIterations < *maxIterations) *maxIterations = curIterations;
			iterators.Append(OpAnd, std::move(selIter));
			was = true;
		}
		if (was) optimized++;
	}
	optimized_ = optimized == joinQuery_.joinEntries_.size();
}

}  // namespace reindexer
