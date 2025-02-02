#include "fieldscomparator.h"

#include <sstream>
#include "core/keyvalue/p_string.h"
#include "core/payload/payloadfieldvalue.h"
#include "core/payload/payloadiface.h"
#include "core/type_consts_helpers.h"
#include "tools/string_regexp_functions.h"

namespace {

class ArrayAdapter {
	class ConstIterator {
	public:
		ConstIterator(const ArrayAdapter &aa, size_t i) noexcept : aa_{aa}, index_{i} {}
		ConstIterator &operator++() noexcept {
			++index_;
			return *this;
		}
		bool operator!=(const ConstIterator &other) const noexcept {
			assertrx(&aa_ == &other.aa_);
			return index_ != other.index_;
		}
		reindexer::Variant operator*() const { return aa_[index_]; }

	private:
		const ArrayAdapter &aa_;
		size_t index_;
	};

public:
	ArrayAdapter(const uint8_t *ptr, size_t l, unsigned size_of, KeyValueType t) noexcept
		: ptr_{ptr}, len_{l}, sizeof_{size_of}, type_{t} {}
	size_t size() const noexcept { return len_; }
	reindexer::Variant operator[](size_t i) const {
		assertrx(i < len_);
		switch (type_) {
			case KeyValueInt64:
				return reindexer::Variant{*reinterpret_cast<const int64_t *>(ptr_ + sizeof_ * i)};
			case KeyValueDouble:
				return reindexer::Variant{*reinterpret_cast<const double *>(ptr_ + sizeof_ * i)};
			case KeyValueString:
				return reindexer::Variant{*reinterpret_cast<const reindexer::p_string *>(ptr_ + sizeof_ * i), false};
			case KeyValueBool:
				return reindexer::Variant{*reinterpret_cast<const bool *>(ptr_ + sizeof_ * i)};
			case KeyValueInt:
				return reindexer::Variant{*reinterpret_cast<const int *>(ptr_ + sizeof_ * i)};
			default:
				throw reindexer::Error{errQueryExec, "Field type %s is not supported for two field comparing",
									   reindexer::KeyValueTypeToStr(type_)};
		}
	}
	ConstIterator begin() const noexcept { return {*this, 0}; }
	ConstIterator end() const noexcept { return {*this, len_}; }

private:
	const uint8_t *ptr_;
	size_t len_;
	unsigned sizeof_;
	KeyValueType type_;
};

}  // namespace

namespace reindexer {

FieldsComparator::FieldsComparator(std::string_view lField, CondType cond, std::string_view rField, PayloadType plType)
	: condition_{cond}, payloadType_{std::move(plType)} {
	switch (condition_) {
		case CondEq:
		case CondLt:
		case CondLe:
		case CondGt:
		case CondGe:
		case CondRange:
		case CondSet:
		case CondAllSet:
		case CondLike:
			break;
		default:
			throw Error{errQueryExec, "Condition %s is not supported for two field comparing", CondTypeToStr(condition_)};
	}
	std::stringstream nameStream;
	nameStream << lField << ' ' << condition_ << ' ' << rField;
	name_ = nameStream.str();
}

template <typename LArr, typename RArr>
bool FieldsComparator::compare(const LArr &lhs, const RArr &rhs) {
	static constexpr bool needCompareTypes{std::is_same_v<LArr, VariantArray> || std::is_same_v<RArr, VariantArray>};
	switch (condition_) {
		case CondRange:
			if (rhs.size() < 2 || rhs[0].Type() == KeyValueNull || rhs[1].Type() == KeyValueNull) {
				throw Error{errQueryExec, "For condition range second field should be an array of 2 values"};
			}
			for (const Variant &v : lhs) {
				if constexpr (needCompareTypes) {
					if (!compareTypes(v.Type(), rhs[0].Type()) || !compareTypes(v.Type(), rhs[1].Type())) continue;
				}
				if (v.RelaxCompare(rhs[0], collateOpts_) >= 0 && v.RelaxCompare(rhs[1], collateOpts_) <= 0) return true;
			}
			return false;
		case CondLike:
			for (const Variant &lv : lhs) {
				for (const Variant &rv : rhs) {
					if (lv.Type() != KeyValueString || rv.Type() != KeyValueString) {
						throw Error{errQueryExec, "For condition LIKE fields should be of string type"};
					}
					if (matchLikePattern(std::string_view(lv), std::string_view(rv))) return true;
				}
			}
			return false;
		case CondAllSet:
			for (const Variant &rv : rhs) {
				if (rv.Type() == KeyValueNull) continue;
				bool found = false;
				for (const Variant &lv : lhs) {
					if (lv.Type() == KeyValueNull) continue;
					if constexpr (needCompareTypes) {
						if (!compareTypes(lv.Type(), rv.Type())) continue;
					}
					if (lv.RelaxCompare(rv, collateOpts_) == 0) {
						found = true;
						break;
					}
				}
				if (!found) return false;
			}
			return true;
		default:
			for (const Variant &lv : lhs) {
				if (lv.Type() == KeyValueNull) continue;
				for (const Variant &rv : rhs) {
					if (rv.Type() == KeyValueNull) continue;
					if constexpr (needCompareTypes) {
						if (!compareTypes(lv.Type(), rv.Type())) continue;
					}
					const int compRes = lv.RelaxCompare(rv, collateOpts_);
					switch (condition_) {
						case CondEq:
						case CondSet:
							if (compRes == 0) return true;
							break;
						case CondLt:
							if (compRes < 0) return true;
							break;
						case CondLe:
							if (compRes <= 0) return true;
							break;
						case CondGt:
							if (compRes > 0) return true;
							break;
						case CondGe:
							if (compRes >= 0) return true;
							break;
						default:
							throw Error{errQueryExec, "Condition %s is not supported for two field comparing", CondTypeToStr(condition_)};
					}
				}
			}
			return false;
	}
}

bool FieldsComparator::Compare(const PayloadValue &item) {
	if (ctx_.size() > 1) {
		for (const auto &c : ctx_) {
			if (!compare(item, c)) return false;
		}
		return true;
	} else {
		return compare(item, ctx_[0]);
	}
}

bool FieldsComparator::compare(const PayloadValue &item, const Context &ctx) {
	bool result;
	if (ctx.lCtx_.fields_.getTagsPathsLength() > 0) {
		VariantArray lhs;
		ConstPayload(payloadType_, item).GetByJsonPath(ctx.lCtx_.fields_.getTagsPath(0), lhs, ctx.lCtx_.type_);
		if (ctx.rCtx_.fields_.getTagsPathsLength() > 0) {
			VariantArray rhs;
			ConstPayload(payloadType_, item).GetByJsonPath(ctx.rCtx_.fields_.getTagsPath(0), rhs, ctx.rCtx_.type_);
			result = compare(lhs, rhs);
		} else if (ctx.rCtx_.isArray_) {
			const PayloadFieldValue::Array *rArr = reinterpret_cast<PayloadFieldValue::Array *>(item.Ptr() + ctx.rCtx_.offset_);
			result = compare(lhs, ArrayAdapter(item.Ptr() + rArr->offset, rArr->len, ctx.rCtx_.sizeof_, ctx.rCtx_.type_));
		} else {
			result = compare(lhs, ArrayAdapter(item.Ptr() + ctx.rCtx_.offset_, 1, ctx.rCtx_.sizeof_, ctx.rCtx_.type_));
		}
	} else if (ctx.rCtx_.fields_.getTagsPathsLength() > 0) {
		VariantArray rhs;
		ConstPayload(payloadType_, item).GetByJsonPath(ctx.rCtx_.fields_.getTagsPath(0), rhs, ctx.rCtx_.type_);
		if (ctx.lCtx_.isArray_) {
			const PayloadFieldValue::Array *lArr = reinterpret_cast<PayloadFieldValue::Array *>(item.Ptr() + ctx.lCtx_.offset_);
			result = compare(ArrayAdapter(item.Ptr() + lArr->offset, lArr->len, ctx.lCtx_.sizeof_, ctx.lCtx_.type_), rhs);
		} else {
			result = compare(ArrayAdapter(item.Ptr() + ctx.lCtx_.offset_, 1, ctx.lCtx_.sizeof_, ctx.lCtx_.type_), rhs);
		}
	} else if (ctx.lCtx_.isArray_) {
		const PayloadFieldValue::Array *lArr = reinterpret_cast<PayloadFieldValue::Array *>(item.Ptr() + ctx.lCtx_.offset_);
		if (ctx.rCtx_.isArray_) {
			const PayloadFieldValue::Array *rArr = reinterpret_cast<PayloadFieldValue::Array *>(item.Ptr() + ctx.rCtx_.offset_);
			result = compare(ArrayAdapter(item.Ptr() + lArr->offset, lArr->len, ctx.lCtx_.sizeof_, ctx.lCtx_.type_),
							 ArrayAdapter(item.Ptr() + rArr->offset, rArr->len, ctx.rCtx_.sizeof_, ctx.rCtx_.type_));
		} else {
			result = compare(ArrayAdapter(item.Ptr() + lArr->offset, lArr->len, ctx.lCtx_.sizeof_, ctx.lCtx_.type_),
							 ArrayAdapter(item.Ptr() + ctx.rCtx_.offset_, 1, ctx.rCtx_.sizeof_, ctx.rCtx_.type_));
		}
	} else if (ctx.rCtx_.isArray_) {
		const PayloadFieldValue::Array *rArr = reinterpret_cast<PayloadFieldValue::Array *>(item.Ptr() + ctx.rCtx_.offset_);
		result = compare(ArrayAdapter(item.Ptr() + ctx.lCtx_.offset_, 1, ctx.lCtx_.sizeof_, ctx.lCtx_.type_),
						 ArrayAdapter(item.Ptr() + rArr->offset, rArr->len, ctx.rCtx_.sizeof_, ctx.rCtx_.type_));
	} else {
		result = compare(ArrayAdapter(item.Ptr() + ctx.lCtx_.offset_, 1, ctx.lCtx_.sizeof_, ctx.lCtx_.type_),
						 ArrayAdapter(item.Ptr() + ctx.rCtx_.offset_, 1, ctx.rCtx_.sizeof_, ctx.rCtx_.type_));
	}
	if (result) ++matchedCount_;
	return result;
}

void FieldsComparator::validateTypes(KeyValueType lType, KeyValueType rType) const {
	if (lType == rType || lType == KeyValueUndefined || rType == KeyValueUndefined) return;
	switch (lType) {
		case KeyValueString:
			throw Error{errQueryExec, "Cannot compare a string field with a non-string one: %s", name_};
		case KeyValueInt:
		case KeyValueInt64:
		case KeyValueDouble:
			if (rType != KeyValueInt && rType != KeyValueInt64 && rType != KeyValueDouble) {
				throw Error{errQueryExec, "Cannot compare a numeric field with a non-numeric one: %s", name_};
			}
			break;
		case KeyValueBool:
			throw Error{errQueryExec, "Cannot compare a boolean field with a non-boolean one: %s", name_};
		default:
			throw Error{errQueryExec, "Field of type %s cannot be compared with another field: %s", KeyValueTypeToStr(lType), name_};
	}
}

}  // namespace reindexer
