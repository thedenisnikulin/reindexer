#include "indexunordered.h"
#include "core/index/indextext/ftkeyentry.h"
#include "core/index/payload_map.h"
#include "core/index/string_map.h"
#include "core/indexdef.h"
#include "core/rdxcontext.h"
#include "rtree/greenesplitter.h"
#include "rtree/linearsplitter.h"
#include "rtree/quadraticsplitter.h"
#include "rtree/rstarsplitter.h"
#include "rtree/rtree.h"
#include "tools/errors.h"
#include "tools/logger.h"

namespace reindexer {

constexpr int kMaxIdsForDistinct = 500;

template <typename T>
IndexUnordered<T>::IndexUnordered(const IndexDef &idef, PayloadType payloadType, const FieldsSet &fields)
	: Base(idef, std::move(payloadType), fields), idx_map() {
	static_assert(!(is_str_map_v<T> || is_payload_map_v<T>));
}

template <>
IndexUnordered<str_map<Index::KeyEntryPlain>>::IndexUnordered(const IndexDef &idef, PayloadType payloadType, const FieldsSet &fields)
	: Base(idef, std::move(payloadType), fields), idx_map(idef.opts_.collateOpts_) {}

template <>
IndexUnordered<str_map<Index::KeyEntry>>::IndexUnordered(const IndexDef &idef, PayloadType payloadType, const FieldsSet &fields)
	: Base(idef, std::move(payloadType), fields), idx_map(idef.opts_.collateOpts_) {}

template <>
IndexUnordered<unordered_str_map<Index::KeyEntry>>::IndexUnordered(const IndexDef &idef, PayloadType payloadType, const FieldsSet &fields)
	: Base(idef, std::move(payloadType), fields), idx_map(idef.opts_.collateOpts_) {}

template <>
IndexUnordered<unordered_str_map<Index::KeyEntryPlain>>::IndexUnordered(const IndexDef &idef, PayloadType payloadType,
																		const FieldsSet &fields)
	: Base(idef, std::move(payloadType), fields), idx_map(idef.opts_.collateOpts_) {}

template <>
IndexUnordered<unordered_str_map<FtKeyEntry>>::IndexUnordered(const IndexDef &idef, PayloadType payloadType, const FieldsSet &fields)
	: Base(idef, std::move(payloadType), fields), idx_map(idef.opts_.collateOpts_) {}

template <>
IndexUnordered<unordered_payload_map<FtKeyEntry, true>>::IndexUnordered(const IndexDef &idef, PayloadType payloadType,
																		const FieldsSet &fields)
	: Base(idef, payloadType, fields), idx_map(std::move(payloadType), fields) {}

template <>
IndexUnordered<unordered_payload_map<Index::KeyEntry, true>>::IndexUnordered(const IndexDef &idef, PayloadType payloadType,
																			 const FieldsSet &fields)
	: Base(idef, payloadType, fields), idx_map(std::move(payloadType), fields) {}

template <>
IndexUnordered<unordered_payload_map<Index::KeyEntryPlain, true>>::IndexUnordered(const IndexDef &idef, PayloadType payloadType,
																				  const FieldsSet &fields)
	: Base(idef, payloadType, fields), idx_map(std::move(payloadType), fields) {}

template <>
IndexUnordered<payload_map<Index::KeyEntry, true>>::IndexUnordered(const IndexDef &idef, PayloadType payloadType, const FieldsSet &fields)
	: Base(idef, payloadType, fields), idx_map(std::move(payloadType), fields) {}

template <>
IndexUnordered<payload_map<Index::KeyEntryPlain, true>>::IndexUnordered(const IndexDef &idef, PayloadType payloadType,
																		const FieldsSet &fields)
	: Base(idef, payloadType, fields), idx_map(std::move(payloadType), fields) {}

template <typename T>
bool IndexUnordered<T>::HoldsStrings() const noexcept {
	if constexpr (is_payload_map_v<T>) {
		return idx_map.have_str_fields();
	}
	return is_str_map_v<T>;
}

template <typename T>
IndexUnordered<T>::IndexUnordered(const IndexUnordered &other)
	: Base(other), idx_map(other.idx_map), cache_(nullptr), empty_ids_(other.empty_ids_), tracker_(other.tracker_) {}

template <typename key_type>
size_t heap_size(const key_type & /*kt*/) {
	return 0;
}

template <>
size_t heap_size<key_string>(const key_string &kt) {
	return kt->heap_size() + sizeof(*kt.get());
}

struct DeepClean {
	template <typename T>
	void operator()(T &v) const {
		free_node(v.first);
		free_node(v.second);
	}

	template <typename T>
	void free_node(T &v) const {
		if constexpr (!std::is_const_v<T>) {
			v = T{};
		}
	}
};

template <typename T>
void IndexUnordered<T>::addMemStat(typename T::iterator it) {
	this->memStat_.idsetPlainSize += sizeof(typename T::value_type) + it->second.Unsorted().heap_size();
	this->memStat_.idsetBTreeSize += it->second.Unsorted().BTreeSize();
	this->memStat_.dataSize += heap_size(it->first);
}

template <typename T>
void IndexUnordered<T>::delMemStat(typename T::iterator it) {
	this->memStat_.idsetPlainSize -= sizeof(typename T::value_type) + it->second.Unsorted().heap_size();
	this->memStat_.idsetBTreeSize -= it->second.Unsorted().BTreeSize();
	this->memStat_.dataSize -= heap_size(it->first);
}

template <typename T>
Variant IndexUnordered<T>::Upsert(const Variant &key, IdType id, bool &clearCache) {
	// reset cache
	if (key.Type() == KeyValueNull) {
		if (this->empty_ids_.Unsorted().Add(id, IdSet::Auto, this->sortedIdxCount_)) {
			if (cache_) cache_.reset();
			clearCache = true;
			this->isBuilt_ = false;
		}
		// Return invalid ref
		return Variant();
	}

	typename T::iterator keyIt = this->idx_map.find(static_cast<ref_type>(key));
	if (keyIt == this->idx_map.end()) {
		keyIt = this->idx_map.insert({static_cast<typename T::key_type>(key), typename T::mapped_type()}).first;
	} else {
		delMemStat(keyIt);
	}

	if (keyIt->second.Unsorted().Add(id, this->opts_.IsPK() ? IdSet::Ordered : IdSet::Auto, this->sortedIdxCount_)) {
		if (cache_) cache_.reset();
		clearCache = true;
		this->isBuilt_ = false;
	}
	this->tracker_.markUpdated(this->idx_map, keyIt);

	addMemStat(keyIt);

	if (this->KeyType() == KeyValueString && this->opts_.GetCollateMode() != CollateNone) {
		return Base::Upsert(key, id, clearCache);
	}

	return Variant(keyIt->first);
}

template <typename T>
void IndexUnordered<T>::Delete(const Variant &key, IdType id, StringsHolder &strHolder, bool &clearCache) {
	int delcnt = 0;
	if (key.Type() == KeyValueNull) {
		delcnt = this->empty_ids_.Unsorted().Erase(id);
		assertrx(delcnt);
		this->isBuilt_ = false;
		if (cache_) cache_.reset();
		clearCache = true;
		return;
	}

	typename T::iterator keyIt = this->idx_map.find(static_cast<ref_type>(key));
	if (keyIt == idx_map.end()) return;

	delMemStat(keyIt);
	delcnt = keyIt->second.Unsorted().Erase(id);
	(void)delcnt;
	this->isBuilt_ = false;
	if (cache_) cache_.reset();
	clearCache = true;
	// TODO: we have to implement removal of composite indexes (doesn't work right now)
	assertf(this->opts_.IsArray() || this->Opts().IsSparse() || delcnt, "Delete unexists id from index '%s' id=%d,key=%s (%s)", this->name_,
			id, key.As<string>(this->payloadType_, this->fields_), Variant(keyIt->first).As<string>(this->payloadType_, this->fields_));

	if (keyIt->second.Unsorted().IsEmpty()) {
		this->tracker_.markDeleted(keyIt);
		if constexpr (is_str_map_v<T>) {
			idx_map.template erase<StringMapEntryCleaner<true>>(
				keyIt, {strHolder, this->KeyType() == KeyValueString && this->opts_.GetCollateMode() == CollateNone});
		} else if constexpr (is_payload_map_v<T>) {
			idx_map.template erase<DeepClean>(keyIt, strHolder);
		} else {
			idx_map.template erase<DeepClean>(keyIt);
		}
	} else {
		addMemStat(keyIt);
		this->tracker_.markUpdated(this->idx_map, keyIt);
	}

	if (this->KeyType() == KeyValueString && this->opts_.GetCollateMode() != CollateNone) {
		Base::Delete(key, id, strHolder, clearCache);
	}
}

template <typename T>
bool IndexUnordered<T>::tryIdsetCache(const VariantArray &keys, CondType condition, SortType sortId,
									  std::function<bool(SelectKeyResult &)> selector, SelectKeyResult &res) {
	if (!cache_ || IsComposite(this->Type())) {
		selector(res);
		return false;
	}
	bool scanWin = false;

	IdSetCacheKey ckey{keys, condition, sortId};
	auto cached = cache_->Get(ckey);
	if (cached.valid) {
		if (!cached.val.ids) {
			scanWin = selector(res);
			if (!scanWin) cache_->Put(ckey, res.mergeIdsets());
		} else {
			res.push_back(SingleSelectKeyResult(cached.val.ids));
		}
	} else {
		scanWin = selector(res);
	}
	return scanWin;
}

template <typename T>
SelectKeyResults IndexUnordered<T>::SelectKey(const VariantArray &keys, CondType condition, SortType sortId, Index::SelectOpts opts,
											  BaseFunctionCtx::Ptr funcCtx, const RdxContext &rdxCtx) {
	const auto indexWard(rdxCtx.BeforeIndexWork());
	if (opts.forceComparator) return Base::SelectKey(keys, condition, sortId, opts, funcCtx, rdxCtx);

	SelectKeyResult res;

	switch (condition) {
		case CondEmpty:
			if (!this->opts_.IsArray() && !this->opts_.IsSparse())
				throw Error(errParams, "The 'is NULL' condition is suported only by 'sparse' or 'array' indexes");
			res.push_back(SingleSelectKeyResult(this->empty_ids_, sortId));
			break;
		// Get set of keys or single key
		case CondEq:
		case CondSet:
			if (condition == CondEq && keys.size() < 1) {
				throw Error(errParams, "For condition required at least 1 argument, but provided 0");
			}
			{
				struct {
					T *i_map;
					const VariantArray &keys;
					SortType sortId;
					Index::SelectOpts opts;
				} ctx = {&this->idx_map, keys, sortId, opts};
				// should return true, if fallback to comparator required
				auto selector = [&ctx](SelectKeyResult &res) -> bool {
					size_t idsCount = 0;
					res.reserve(ctx.keys.size());
					for (auto key : ctx.keys) {
						auto keyIt = ctx.i_map->find(static_cast<ref_type>(key));
						if (keyIt != ctx.i_map->end()) {
							res.emplace_back(keyIt->second, ctx.sortId);
							idsCount += keyIt->second.Unsorted().Size();
						}
					}
					if (!ctx.opts.itemsCountInNamespace) return false;
					// if (ctx.opts.indexesNotOptimized) idsCount *= 2;
					// Check selectivity:
					// if ids count too much (more than 20% of namespace),
					// and index not optimized, or we have >4 other conditions

					return res.size() > 1u && ((int(idsCount * 2) > ctx.opts.maxIterations) ||
											   (100u * idsCount / ctx.opts.itemsCountInNamespace > maxSelectivityPercentForIdset()));
				};

				bool scanWin = false;
				// Get from cache
				if (!opts.distinct && !opts.disableIdSetCache && keys.size() > 1) {
					scanWin = tryIdsetCache(keys, condition, sortId, selector, res);
				} else
					scanWin = selector(res);

				if (scanWin && !opts.distinct) {
					// fallback to comparator, due to expensive idset
					return Base::SelectKey(keys, condition, sortId, opts, funcCtx, rdxCtx);
				}
			}
			break;
		case CondAllSet: {
			// Get set of key, where all request keys are present
			SelectKeyResults rslts;
			for (auto key : keys) {
				SelectKeyResult res1;
				key.convert(this->KeyType());
				auto keyIt = this->idx_map.find(static_cast<ref_type>(key));
				if (keyIt == this->idx_map.end()) {
					rslts.clear();
					rslts.push_back(res1);
					return rslts;
				}
				res1.push_back(SingleSelectKeyResult(keyIt->second, sortId));
				rslts.push_back(res1);
			}
			return rslts;
		}

		case CondAny:
			if (opts.distinct && this->idx_map.size() < kMaxIdsForDistinct) {  // TODO change to more clever condition
				// Get set of any keys
				res.reserve(this->idx_map.size());
				for (auto &keyIt : this->idx_map) res.emplace_back(keyIt.second, sortId);
				break;
			}  // else fallthrough
		case CondGe:
		case CondLe:
		case CondRange:
		case CondGt:
		case CondLt:
		case CondLike:
			return Base::SelectKey(keys, condition, sortId, opts, funcCtx, rdxCtx);
		default:
			throw Error(errQueryExec, "Unknown query on index '%s'", this->name_);
	}

	return SelectKeyResults(std::move(res));
}

template <typename T>
void IndexUnordered<T>::Commit() {
	this->empty_ids_.Unsorted().Commit();

	if (!cache_) cache_.reset(new IdSetCache());

	if (!tracker_.isUpdated()) return;

	logPrintf(LogTrace, "IndexUnordered::Commit (%s) %d uniq keys, %d empty, %s", this->name_, this->idx_map.size(),
			  this->empty_ids_.Unsorted().size(), tracker_.isCompleteUpdated() ? "complete" : "partial");

	if (tracker_.isCompleteUpdated()) {
		for (auto &keyIt : this->idx_map) {
			keyIt.second.Unsorted().Commit();
			assertrx(keyIt.second.Unsorted().size());
		}
	} else {
		tracker_.commitUpdated(idx_map);
	}
	tracker_.clear();
}

template <typename T>
void IndexUnordered<T>::UpdateSortedIds(const UpdateSortedContext &ctx) {
	logPrintf(LogTrace, "IndexUnordered::UpdateSortedIds (%s) %d uniq keys, %d empty", this->name_, this->idx_map.size(),
			  this->empty_ids_.Unsorted().size());
	// For all keys in index
	for (auto &keyIt : this->idx_map) {
		keyIt.second.UpdateSortedIds(ctx);
	}

	this->empty_ids_.UpdateSortedIds(ctx);
}

template <typename T>
std::unique_ptr<Index> IndexUnordered<T>::Clone() {
	return std::unique_ptr<Index>{new IndexUnordered<T>(*this)};
}

template <typename T>
void IndexUnordered<T>::SetSortedIdxCount(int sortedIdxCount) {
	if (this->sortedIdxCount_ != sortedIdxCount) {
		this->sortedIdxCount_ = sortedIdxCount;
		for (auto &keyIt : idx_map) keyIt.second.Unsorted().ReserveForSorted(this->sortedIdxCount_);
	}
}

template <typename T>
IndexMemStat IndexUnordered<T>::GetMemStat() {
	IndexMemStat ret = Base::GetMemStat();
	ret.uniqKeysCount = idx_map.size();
	if (cache_) ret.idsetCache = cache_->GetMemStat();
	ret.trackedUpdatesCount = tracker_.updatesSize();
	ret.trackedUpdatesBuckets = tracker_.updatesBuckets();
	return ret;
}

template <typename T>
template <typename S>
void IndexUnordered<T>::dump(S &os, std::string_view step, std::string_view offset) const {
	std::string newOffset{offset};
	newOffset += step;
	os << "{\n" << newOffset << "<IndexStore>: ";
	Base::Dump(os, step, newOffset);
	os << ",\n" << newOffset << "idx_map: {";
	if (!idx_map.empty()) {
		std::string secondOffset{newOffset};
		secondOffset += step;
		for (auto b = idx_map.begin(), it = b, e = idx_map.end(); it != e; ++it) {
			if (it != b) os << ',';
			os << '\n' << secondOffset << '{' << it->first << ": ";
			it->second.Dump(os, step, secondOffset);
			os << '}';
		}
		os << '\n' << newOffset;
	}
	os << "},\n" << newOffset << "cache: ";
	if (cache_) {
		cache_->Dump(os, step, newOffset);
	} else {
		os << "empty";
	}
	os << ",\n" << newOffset << "empty_ids: ";
	empty_ids_.Dump(os, step, newOffset);
	os << "\n" << offset << '}';
}

template <typename T>
void IndexUnordered<T>::Dump(std::ostream &os, std::string_view step, std::string_view offset) const {
	dump(os, step, offset);
}

template <typename KeyEntryT>
static std::unique_ptr<Index> IndexUnordered_New(const IndexDef &idef, PayloadType payloadType, const FieldsSet &fields) {
	switch (idef.Type()) {
		case IndexIntHash:
			return std::unique_ptr<Index>{new IndexUnordered<unordered_number_map<int, KeyEntryT>>(idef, std::move(payloadType), fields)};
		case IndexInt64Hash:
			return std::unique_ptr<Index>{
				new IndexUnordered<unordered_number_map<int64_t, KeyEntryT>>(idef, std::move(payloadType), fields)};
		case IndexStrHash:
			return std::unique_ptr<Index>{new IndexUnordered<unordered_str_map<KeyEntryT>>(idef, std::move(payloadType), fields)};
		case IndexCompositeHash:
			return std::unique_ptr<Index>{new IndexUnordered<unordered_payload_map<KeyEntryT, true>>(idef, std::move(payloadType), fields)};
		default:
			abort();
	}
}

std::unique_ptr<Index> IndexUnordered_New(const IndexDef &idef, PayloadType payloadType, const FieldsSet &fields) {
	return (idef.opts_.IsPK() || idef.opts_.IsDense()) ? IndexUnordered_New<Index::KeyEntryPlain>(idef, std::move(payloadType), fields)
													   : IndexUnordered_New<Index::KeyEntry>(idef, std::move(payloadType), fields);
}

template class IndexUnordered<number_map<int, Index::KeyEntryPlain>>;
template class IndexUnordered<number_map<int64_t, Index::KeyEntryPlain>>;
template class IndexUnordered<number_map<double, Index::KeyEntryPlain>>;
template class IndexUnordered<str_map<Index::KeyEntryPlain>>;
template class IndexUnordered<payload_map<Index::KeyEntryPlain, true>>;
template class IndexUnordered<number_map<int, Index::KeyEntry>>;
template class IndexUnordered<number_map<int64_t, Index::KeyEntry>>;
template class IndexUnordered<number_map<double, Index::KeyEntry>>;
template class IndexUnordered<str_map<Index::KeyEntry>>;
template class IndexUnordered<payload_map<Index::KeyEntry, true>>;
template class IndexUnordered<unordered_str_map<FtKeyEntry>>;
template class IndexUnordered<unordered_payload_map<FtKeyEntry, true>>;
template class IndexUnordered<GeometryMap<Index::KeyEntry, LinearSplitter, 32, 4>>;
template class IndexUnordered<GeometryMap<Index::KeyEntryPlain, LinearSplitter, 32, 4>>;
template class IndexUnordered<GeometryMap<Index::KeyEntry, QuadraticSplitter, 32, 4>>;
template class IndexUnordered<GeometryMap<Index::KeyEntryPlain, QuadraticSplitter, 32, 4>>;
template class IndexUnordered<GeometryMap<Index::KeyEntry, GreeneSplitter, 16, 4>>;
template class IndexUnordered<GeometryMap<Index::KeyEntryPlain, GreeneSplitter, 16, 4>>;
template class IndexUnordered<GeometryMap<Index::KeyEntry, RStarSplitter, 32, 4>>;
template class IndexUnordered<GeometryMap<Index::KeyEntryPlain, RStarSplitter, 32, 4>>;

}  // namespace reindexer
