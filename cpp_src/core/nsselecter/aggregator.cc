#include "aggregator.h"
#include <algorithm>
#include <limits>
#include "core/queryresults/queryresults.h"
#include "estl/overloaded.h"

namespace reindexer {

template <typename It>
static void moveFrames(It &begin, It &end, size_t size, size_t offset, size_t limit) {
	if (offset > 0) {
		std::advance(begin, offset);
	}
	if (limit != UINT_MAX && offset + limit < size) {
		end = begin;
		std::advance(end, limit);
	}
}

template <typename It>
static void copy(It begin, It end, std::vector<FacetResult> &facets, const FieldsSet &fields, const PayloadType &payloadType) {
	for (; begin != end; ++begin) {
		facets.push_back({{}, begin->second});
		int tagPathIdx = 0;
		for (size_t i = 0; i < fields.size(); ++i) {
			ConstPayload pl(payloadType, begin->first);
			VariantArray va;
			if (fields[i] == IndexValueType::SetByJsonPath) {
				const TagsPath &tagsPath = fields.getTagsPath(tagPathIdx++);
				pl.GetByJsonPath(tagsPath, va, KeyValueUndefined);
				if (va.IsObjectValue()) {
					throw Error(errQueryExec, "Cannot aggregate object field");
				}
			} else {
				pl.Get(fields[i], va);
			}
			facets.back().values.push_back(va.empty() ? string() : va.front().As<string>());
		}
	}
}

template <typename It>
static void copy(It begin, It end, std::vector<FacetResult> &facets) {
	for (; begin != end; ++begin) {
		facets.push_back({{begin->first.template As<string>()}, begin->second});
	}
}

class Aggregator::MultifieldComparator {
public:
	MultifieldComparator(const h_vector<SortingEntry, 1> &, const FieldsSet &, const PayloadType &);
	bool HaveCompareByCount() const { return haveCompareByCount; }
	bool operator()(const PayloadValue &lhs, const PayloadValue &rhs) const;
	bool operator()(const pair<PayloadValue, int> &lhs, const pair<PayloadValue, int> &rhs) const;

private:
	struct CompOpts {
		CompOpts() : direction{Asc} {}
		CompOpts(const FieldsSet &fs, Direction d) : fields{fs}, direction{d} {}
		FieldsSet fields;  // if empty - compare by count
		Direction direction = Asc;
	};
	h_vector<CompOpts, 2> compOpts_;
	PayloadType type_;
	bool haveCompareByCount = false;

	void insertField(size_t toIdx, const FieldsSet &from, size_t fromIdx, int &tagsPathIdx);
};

class Aggregator::SinglefieldComparator {
	enum CompareBy { ByValue, ByCount };

public:
	SinglefieldComparator(const h_vector<SortingEntry, 1> &);
	bool HaveCompareByCount() const { return haveCompareByCount; }
	bool operator()(const Variant &lhs, const Variant &rhs) const { return lhs.Compare(rhs) * valueCompareDirection_ < 0; }
	bool operator()(const pair<Variant, int> &lhs, const pair<Variant, int> &rhs) const;

private:
	struct CompOpts {
		CompOpts() = default;
		CompOpts(CompareBy compBy, Direction direc) : compareBy(compBy), direction(direc) {}
		CompareBy compareBy;
		Direction direction;
	};
	h_vector<CompOpts, 1> compOpts_;
	Direction valueCompareDirection_;
	bool haveCompareByCount = false;
};

Aggregator::MultifieldComparator::MultifieldComparator(const h_vector<SortingEntry, 1> &sortingEntries, const FieldsSet &fields,
													   const PayloadType &type)
	: compOpts_{}, type_{type}, haveCompareByCount{false} {
	if (sortingEntries.empty()) {
		compOpts_.emplace_back(fields, Asc);
		return;
	}

	compOpts_.resize(sortingEntries.size() + 1);
	int tagsPathIdx = 0;
	for (size_t i = 0; i < fields.size(); ++i) {
		size_t j = 0;
		for (; j < sortingEntries.size(); ++j) {
			if (static_cast<int>(i) == sortingEntries[j].field) {
				insertField(j, fields, i, tagsPathIdx);
				compOpts_[j].direction = sortingEntries[j].desc ? Desc : Asc;
				break;
			}
		}
		if (j == sortingEntries.size()) {
			insertField(j, fields, i, tagsPathIdx);
		}
	}
	if (compOpts_.size() > 1 && compOpts_.back().fields.empty()) {
		auto end = compOpts_.end();
		compOpts_.erase(--end);
	}
	for (const auto &opt : compOpts_) {
		if (opt.fields.empty()) {
			haveCompareByCount = true;
			break;
		}
	}
}

bool Aggregator::MultifieldComparator::operator()(const PayloadValue &lhs, const PayloadValue &rhs) const {
	for (const auto &opt : compOpts_) {
		if (opt.fields.empty()) continue;
		assertrx(type_);
		assertrx(!lhs.IsFree());
		assertrx(!rhs.IsFree());
		int less = ConstPayload(type_, lhs).Compare(rhs, opt.fields);
		if (less == 0) continue;
		return less * opt.direction < 0;
	}
	return false;
}

bool Aggregator::MultifieldComparator::operator()(const pair<PayloadValue, int> &lhs, const pair<PayloadValue, int> &rhs) const {
	for (const auto &opt : compOpts_) {
		if (opt.fields.empty()) {
			if (lhs.second == rhs.second) continue;
			return opt.direction * (lhs.second - rhs.second) < 0;
		}
		assertrx(type_);
		assertrx(!lhs.first.IsFree());
		assertrx(!rhs.first.IsFree());
		int less = ConstPayload(type_, lhs.first).Compare(rhs.first, opt.fields);
		if (less == 0) continue;
		return less * opt.direction < 0;
	}
	return false;
}

void Aggregator::MultifieldComparator::insertField(size_t toIdx, const FieldsSet &from, size_t fromIdx, int &tagsPathIdx) {
	compOpts_[toIdx].fields.push_back(from[fromIdx]);
	if (from[fromIdx] == IndexValueType::SetByJsonPath) {
		compOpts_[toIdx].fields.push_back(from.getTagsPath(tagsPathIdx++));
	}
}

struct Aggregator::MultifieldOrderedMap : public btree::btree_map<PayloadValue, int, Aggregator::MultifieldComparator> {
	using Base = btree::btree_map<PayloadValue, int, MultifieldComparator>;
	using Base::Base;
	MultifieldOrderedMap() = delete;
};

Aggregator::SinglefieldComparator::SinglefieldComparator(const h_vector<SortingEntry, 1> &sortingEntries)
	: valueCompareDirection_(Asc), haveCompareByCount(false) {
	bool haveCompareByValue = false;
	for (const SortingEntry &sortEntry : sortingEntries) {
		CompareBy compareBy = ByValue;
		Direction direc = sortEntry.desc ? Desc : Asc;
		if (sortEntry.field == SortingEntry::Count) {
			compareBy = ByCount;
			haveCompareByCount = true;
		} else {
			valueCompareDirection_ = direc;
			haveCompareByValue = true;
		}
		compOpts_.emplace_back(compareBy, direc);
	}
	if (!haveCompareByValue) {
		compOpts_.emplace_back(ByValue, valueCompareDirection_);
	}
}

bool Aggregator::SinglefieldComparator::operator()(const pair<Variant, int> &lhs, const pair<Variant, int> &rhs) const {
	for (const CompOpts &opt : compOpts_) {
		int less;
		if (opt.compareBy == ByValue) {
			less = lhs.first.Compare(rhs.first);
		} else {
			less = lhs.second - rhs.second;
		}
		if (less != 0) return less * opt.direction < 0;
	}
	return false;
}

Aggregator::Aggregator() = default;
Aggregator::Aggregator(Aggregator &&) = default;
Aggregator::~Aggregator() = default;

Aggregator::Aggregator(const PayloadType &payloadType, const FieldsSet &fields, AggType aggType, const h_vector<string, 1> &names,
					   const h_vector<SortingEntry, 1> &sort, size_t limit, size_t offset, bool compositeIndexFields)
	: payloadType_(payloadType),
	  fields_(fields),
	  aggType_(aggType),
	  names_(names),
	  limit_(limit),
	  offset_(offset),
	  compositeIndexFields_(compositeIndexFields) {
	switch (aggType_) {
		case AggFacet:
			if (fields_.size() == 1) {
				if (sort.empty()) {
					facets_ = std::make_unique<Facets>(SinglefieldUnorderedMap{});
				} else {
					facets_ = std::make_unique<Facets>(SinglefieldOrderedMap{SinglefieldComparator{sort}});
				}
			} else {
				if (sort.empty()) {
					facets_ = std::make_unique<Facets>(MultifieldUnorderedMap{payloadType_, fields_});
				} else {
					facets_ = std::make_unique<Facets>(MultifieldOrderedMap{MultifieldComparator{sort, fields_, payloadType_}});
				}
			}
			break;
		case AggDistinct:
			distincts_.reset(new HashSetVariantRelax(16, DistinctHasher(payloadType, fields), RelaxVariantCompare(payloadType, fields)));
			break;
		case AggMin:
			result_ = std::numeric_limits<double>::max();
			break;
		case AggMax:
			result_ = std::numeric_limits<double>::min();
			break;
		case AggAvg:
		case AggSum:
			break;
		default:
			throw Error(errParams, "Unknown aggregation type %d", aggType_);
	}
}

template <typename FacetMap, typename... Args>
static void fillOrderedFacetResult(std::vector<FacetResult> &result, const FacetMap &facets, size_t offset, size_t limit,
								   const Args &...args) {
	if (offset >= static_cast<size_t>(facets.size())) return;
	result.reserve(std::min(limit, facets.size() - offset));
	const auto &comparator = facets.key_comp();
	if (comparator.HaveCompareByCount()) {
		vector<pair<typename FacetMap::key_type, int>> tmpFacets(facets.begin(), facets.end());
		auto begin = tmpFacets.begin();
		auto end = tmpFacets.end();
		moveFrames(begin, end, tmpFacets.size(), offset, limit);
		std::nth_element(tmpFacets.begin(), begin, tmpFacets.end(), comparator);
		std::partial_sort(begin, end, tmpFacets.end(), comparator);
		copy(begin, end, result, args...);
	} else {
		auto begin = facets.begin();
		auto end = facets.end();
		moveFrames(begin, end, facets.size(), offset, limit);
		copy(begin, end, result, args...);
	}
}

template <typename FacetMap, typename... Args>
static void fillUnorderedFacetResult(std::vector<FacetResult> &result, const FacetMap &facets, size_t offset, size_t limit,
									 const Args &...args) {
	if (offset >= static_cast<size_t>(facets.size())) return;
	result.reserve(std::min(limit, facets.size() - offset));
	auto begin = facets.begin();
	auto end = facets.end();
	moveFrames(begin, end, facets.size(), offset, limit);
	copy(begin, end, result, args...);
}

AggregationResult Aggregator::GetResult() const {
	AggregationResult ret;
	ret.fields = names_;
	ret.type = aggType_;

	switch (aggType_) {
		case AggAvg:
			ret.value = double(hitCount_ == 0 ? 0 : (result_ / hitCount_));
			break;
		case AggSum:
		case AggMin:
		case AggMax:
			ret.value = result_;
			break;
		case AggFacet:
			std::visit(overloaded{[&](const SinglefieldOrderedMap &fm) { fillOrderedFacetResult(ret.facets, fm, offset_, limit_); },
								  [&](const SinglefieldUnorderedMap &fm) { fillUnorderedFacetResult(ret.facets, fm, offset_, limit_); },
								  [&](const MultifieldOrderedMap &fm) {
									  fillOrderedFacetResult(ret.facets, fm, offset_, limit_, fields_, payloadType_);
								  },
								  [&](const MultifieldUnorderedMap &fm) {
									  fillUnorderedFacetResult(ret.facets, fm, offset_, limit_, fields_, payloadType_);
								  }},
					   *facets_);
			break;
		case AggDistinct:
			assertrx(distincts_);
			ret.payloadType = payloadType_;
			ret.distinctsFields = fields_;
			ret.distincts.reserve(distincts_->size());
			for (const Variant &value : *distincts_) {
				ret.distincts.push_back(value);
			}
			break;
		default:
			abort();
	}
	return ret;
}

void Aggregator::Aggregate(const PayloadValue &data) {
	if (aggType_ == AggFacet) {
		const bool done =
			std::visit(overloaded{[&data](MultifieldUnorderedMap &fm) {
									  ++fm[data];
									  return true;
								  },
								  [&data](MultifieldOrderedMap &fm) {
									  ++fm[data];
									  return true;
								  },
								  [](SinglefieldOrderedMap &) { return false; }, [](SinglefieldUnorderedMap &) { return false; }},
					   *facets_);
		if (done) return;
	}
	if (aggType_ == AggDistinct && compositeIndexFields_) {
		aggregate(Variant(data));
		return;
	}

	assertrx(fields_.size() == 1);
	if (fields_[0] == IndexValueType::SetByJsonPath) {
		ConstPayload pl(payloadType_, data);
		VariantArray va;
		const TagsPath &tagsPath = fields_.getTagsPath(0);
		pl.GetByJsonPath(tagsPath, va, KeyValueUndefined);
		if (va.IsObjectValue()) {
			throw Error(errQueryExec, "Cannot aggregate object field");
		}
		for (const Variant &v : va) aggregate(v);
		return;
	}

	const auto &fieldType = payloadType_.Field(fields_[0]);
	if (!fieldType.IsArray()) {
		aggregate(PayloadFieldValue(fieldType, data.Ptr() + fieldType.Offset()).Get());
		return;
	}

	PayloadFieldValue::Array *arr = reinterpret_cast<PayloadFieldValue::Array *>(data.Ptr() + fieldType.Offset());

	uint8_t *ptr = data.Ptr() + arr->offset;
	for (int i = 0; i < arr->len; i++, ptr += fieldType.ElemSizeof()) {
		aggregate(PayloadFieldValue(fieldType, ptr).Get());
	}
}

void Aggregator::aggregate(const Variant &v) {
	switch (aggType_) {
		case AggSum:
		case AggAvg:
			result_ += v.As<double>();
			hitCount_++;
			break;
		case AggMin:
			result_ = std::min(v.As<double>(), result_);
			break;
		case AggMax:
			result_ = std::max(v.As<double>(), result_);
			break;
		case AggFacet:
			std::visit(overloaded{[&v](SinglefieldUnorderedMap &fm) { ++fm[v]; }, [&v](SinglefieldOrderedMap &fm) { ++fm[v]; },
								  [](MultifieldUnorderedMap &) { assertrx(0); }, [](MultifieldOrderedMap &) { assertrx(0); }},
					   *facets_);
			break;
		case AggDistinct:
			assertrx(distincts_);
			distincts_->insert(v);
			break;
		case AggUnknown:
		case AggCount:
		case AggCountCached:
			break;
	};
}

}  // namespace reindexer
