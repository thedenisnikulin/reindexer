#pragma once

#include "core/keyvalue/p_string.h"
#include "core/payload/payloadiface.h"
#include "core/query/query.h"
#include "estl/h_vector.h"

namespace reindexer {

struct NsContext;
class NamespaceImpl;
class ExpressionEvaluator;

class ItemModifier {
public:
	ItemModifier(const h_vector<UpdateEntry, 0> &, NamespaceImpl &ns);
	ItemModifier(const ItemModifier &) = delete;
	ItemModifier &operator=(const ItemModifier &) = delete;
	ItemModifier(ItemModifier &&) = delete;
	ItemModifier &operator=(ItemModifier &&) = delete;

	void Modify(IdType itemId, const NsContext &ctx);

private:
	struct FieldData {
		FieldData(const UpdateEntry &entry, NamespaceImpl &ns);
		void updateTagsPath(TagsMatcher &tm, const IndexExpressionEvaluator &ev);
		const UpdateEntry &details() const noexcept { return entry_; }
		const IndexedTagsPath &tagspath() const noexcept { return tagsPath_; }
		int arrayIndex() const noexcept { return arrayIndex_; }
		int index() const noexcept { return fieldIndex_; }
		bool isIndex() const noexcept { return isIndex_; }
		const string &name() const noexcept { return entry_.column; }
		const string &jsonpath() const noexcept { return jsonPath_; }

	private:
		const UpdateEntry &entry_;
		IndexedTagsPath tagsPath_;
		string jsonPath_;
		int fieldIndex_;
		int arrayIndex_;
		bool isIndex_;
	};
	struct CJsonCache {
		CJsonCache() = default;
		void Assign(std::string_view str) {
			if (data_.capacity() < DefaultCJsonSize) {
				data_.reserve(DefaultCJsonSize);
			}
			data_.assign(str.begin(), str.end());
			cjson_ = std::string_view(&data_[0], str.size());
		}
		p_string Get() const { return p_string(&cjson_); }
		void Reset() { cjson_ = {nullptr, 0}; }
		int Size() const { return cjson_.length(); }

	private:
		enum { DefaultCJsonSize = 4096 };
		std::vector<char> data_;
		std::string_view cjson_;
	};

	void modifyField(IdType itemId, FieldData &field, Payload &pl, VariantArray &values, const NsContext &);
	void modifyCJSON(PayloadValue &pv, IdType itemId, FieldData &field, VariantArray &values, const NsContext &);
	void modifyIndexValues(IdType itemId, const FieldData &field, VariantArray &values, Payload &pl, const NsContext &);

	NamespaceImpl &ns_;
	const h_vector<UpdateEntry, 0> &updateEntries_;
	vector<FieldData> fieldsToModify_;
	CJsonCache cjsonCache_;
};

}  // namespace reindexer
