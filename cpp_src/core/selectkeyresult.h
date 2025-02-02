#pragma once

#include <climits>
#include <memory>

#include "core/comparator.h"
#include "core/idset.h"
#include "core/index/indexiterator.h"
#include "index/keyentry.h"

namespace reindexer {

/// Stores result of selecting data for only 1 value of
/// a certain key. i.e. for condition "A>=10 && A<20" it
/// contains only 1 IdSet for one of the following keys:
/// 10, 11, 12, 13, ... 19 (For example all rowIds for
/// the value '10').
class SingleSelectKeyResult {
	friend class SelectIterator;
	friend class SelectKeyResult;

public:
	SingleSelectKeyResult() {}
	SingleSelectKeyResult(IndexIterator::Ptr indexForwardIter) : indexForwardIter_(indexForwardIter) {
		assertrx(indexForwardIter_ != nullptr);
	}
	template <typename KeyEntryT>
	explicit SingleSelectKeyResult(const KeyEntryT &ids, SortType sortId) {
		if (ids.Unsorted().IsCommited()) {
			ids_ = ids.Sorted(sortId);
		} else {
			assertrx(ids.Unsorted().BTree());
			assertrx(!sortId);
			set_ = ids.Unsorted().BTree();
			useBtree_ = true;
		}
	}
	explicit SingleSelectKeyResult(IdSet::Ptr ids) : tempIds_(ids), ids_(*ids) {}
	explicit SingleSelectKeyResult(const IdSetRef &ids) : ids_(ids) {}
	explicit SingleSelectKeyResult(IdType rBegin, IdType rEnd) : rBegin_(rBegin), rEnd_(rEnd), isRange_(true) {}
	SingleSelectKeyResult(const SingleSelectKeyResult &other)
		: tempIds_(other.tempIds_),
		  ids_(other.ids_),
		  set_(other.set_),
		  indexForwardIter_(other.indexForwardIter_),
		  bsearch_(other.bsearch_),
		  isRange_(other.isRange_),
		  useBtree_(other.useBtree_) {
		if (isRange_) {
			rBegin_ = other.rBegin_;
			rEnd_ = other.rEnd_;
			rIt_ = other.rIt_;
		} else {
			if (useBtree_) {
				setbegin_ = other.setbegin_;
				setrbegin_ = other.setrbegin_;
				setend_ = other.setend_;
				setrend_ = other.setrend_;
				itset_ = other.itset_;
				ritset_ = other.ritset_;
			} else {
				begin_ = other.begin_;
				end_ = other.end_;
				it_ = other.it_;
			}
		}
	}
	SingleSelectKeyResult &operator=(const SingleSelectKeyResult &other) {
		if (&other != this) {
			tempIds_ = other.tempIds_;
			ids_ = other.ids_;
			set_ = other.set_;
			indexForwardIter_ = other.indexForwardIter_;
			bsearch_ = other.bsearch_;
			isRange_ = other.isRange_;
			useBtree_ = other.useBtree_;
			if (isRange_) {
				rBegin_ = other.rBegin_;
				rEnd_ = other.rEnd_;
				rIt_ = other.rIt_;
			} else {
				if (useBtree_) {
					setbegin_ = other.setbegin_;
					setrbegin_ = other.setrbegin_;
					setend_ = other.setend_;
					setrend_ = other.setrend_;
					itset_ = other.itset_;
					ritset_ = other.ritset_;
				} else {
					begin_ = other.begin_;
					end_ = other.end_;
					it_ = other.it_;
				}
			}
		}
		return *this;
	}

	IdSet::Ptr tempIds_;
	IdSetRef ids_;

protected:
	const base_idsetset *set_ = nullptr;

	union {
		IdSetRef::const_iterator begin_;
		IdSetRef::const_reverse_iterator rbegin_;
		base_idsetset::const_iterator setbegin_;
		base_idsetset::const_reverse_iterator setrbegin_;
		int rBegin_ = 0;
		int rrBegin_;
	};

	union {
		IdSetRef::const_iterator end_;
		IdSetRef::const_reverse_iterator rend_;
		base_idsetset::const_iterator setend_;
		base_idsetset::const_reverse_iterator setrend_;
		int rEnd_ = 0;
		int rrEnd_;
	};

	union {
		IdSetRef::const_iterator it_;
		IdSetRef::const_reverse_iterator rit_;
		base_idsetset::const_iterator itset_;
		base_idsetset::const_reverse_iterator ritset_;
		int rIt_ = 0;
		int rrIt_;
	};

	IndexIterator::Ptr indexForwardIter_;

	// if isRange is true then bsearch is always false
	bool bsearch_ = false;
	bool isRange_ = false;
	bool useBtree_ = false;
};

/// Stores results of selecting data for 1 certain key,
/// i.e. for condition "A>=10 && A<20" there will be
/// 10 SingleSelectKeyResult objects (for each of the
/// following keys: 10, 11, 12, 13, ... 19).
class SelectKeyResult : public h_vector<SingleSelectKeyResult, 1> {
public:
	std::vector<Comparator> comparators_;

	void ClearDistinct() {
		for (Comparator &comp : comparators_) comp.ClearDistinct();
	}
	/// Returns total amount of rowIds in all
	/// the SingleSelectKeyResult objects, i.e.
	/// maximum amonut of possible iterations.
	/// @return amount of loops.
	size_t GetMaxIterations(size_t limitIters = std::numeric_limits<size_t>::max()) const {
		size_t cnt = 0;
		for (const SingleSelectKeyResult &r : *this) {
			if (r.indexForwardIter_) {
				cnt += r.indexForwardIter_->GetMaxIterations(limitIters);
			} else if (r.isRange_) {
				cnt += std::abs(r.rEnd_ - r.rBegin_);
			} else if (r.useBtree_) {
				cnt += r.set_->size();
			} else {
				cnt += r.ids_.size();
			}
			if (cnt > limitIters) break;
		}
		return cnt;
	}

	/// Represents data as one sorted set.
	/// Creates 1 set from all the inner
	/// SingleSelectKeyResult objects. Such
	/// representation makes further work with
	/// the object much easier.
	/// @return Pointer to a sorted IdSet object made
	/// from all the SingleSelectKeyResult inner objects.
	IdSet::Ptr mergeIdsets() {
		auto mergedIds = make_intrusive<intrusive_atomic_rc_wrapper<IdSet>>();

		size_t expectSize = 0;
		for (auto it = begin(); it != end(); it++) {
			if (it->useBtree_) {
				it->itset_ = it->set_->begin();
				expectSize += it->set_->size();
			} else {
				it->it_ = it->ids_.begin();
				expectSize += it->ids_.size();
			}
		}
		mergedIds->reserve(expectSize);

		for (;;) {
			const int min = mergedIds->size() ? mergedIds->back() : INT_MIN;
			int curMin = INT_MAX;
			for (auto it = begin(); it != end(); it++) {
				if (it->useBtree_) {
					for (; it->itset_ != it->set_->end() && *it->itset_ <= min; it->itset_++) {
					};
					if (it->itset_ != it->set_->end() && *it->itset_ < curMin) curMin = *it->itset_;
				} else {
					for (; it->it_ != it->ids_.end() && *it->it_ <= min; it->it_++) {
					};
					if (it->it_ != it->ids_.end() && *it->it_ < curMin) curMin = *it->it_;
				}
			}
			if (curMin == INT_MAX) break;
			mergedIds->Add(curMin, IdSet::Unordered, 0);
		};
		mergedIds->shrink_to_fit();
		clear();
		push_back(SingleSelectKeyResult(mergedIds));
		return mergedIds;
	}
};

/// Result of selecting data for
/// each key in a query.
class SelectKeyResults : public h_vector<SelectKeyResult, 1> {
public:
	SelectKeyResults(std::initializer_list<SelectKeyResult> l) { insert(end(), l.begin(), l.end()); }
	SelectKeyResults(SelectKeyResult &&res) { push_back(std::move(res)); }
	SelectKeyResults() {}
};

}  // namespace reindexer
