#pragma once

#include <deque>
#include <vector>

#include "core/cjson/msgpackdecoder.h"
#include "core/cjson/tagsmatcher.h"
#include "core/keyvalue/key_string.h"
#include "core/keyvalue/variant.h"
#include "core/payload/payloadiface.h"
#include "tools/serializer.h"

using std::vector;

namespace reindexer {

struct ItemImplRawData {
	ItemImplRawData() = default;
	ItemImplRawData(PayloadValue v) : payloadValue_(std::move(v)) {}
	ItemImplRawData(const ItemImplRawData &) = delete;
	ItemImplRawData(ItemImplRawData &&) = default;
	ItemImplRawData &operator=(const ItemImplRawData &) = delete;
	ItemImplRawData &operator=(ItemImplRawData &&) = default;

	PayloadValue payloadValue_;
	std::unique_ptr<uint8_t[]> tupleData_;
	std::unique_ptr<char[]> sourceData_;
	vector<string> precepts_;
	std::unique_ptr<std::deque<std::string>> holder_;
	std::unique_ptr<std::vector<key_string>> keyStringsHolder_;
};

class Namespace;
class Schema;

class ItemImpl : public ItemImplRawData {
public:
	ItemImpl() = default;

	// Construct empty item
	ItemImpl(PayloadType type, const TagsMatcher &tagsMatcher, const FieldsSet &pkFields = {}, std::shared_ptr<const Schema> schema = {})
		: ItemImplRawData(PayloadValue(type.TotalSize(), 0, type.TotalSize() + 0x100)),
		  payloadType_(type),
		  tagsMatcher_(tagsMatcher),
		  pkFields_(pkFields),
		  schema_(std::move(schema)) {
		tagsMatcher_.clearUpdated();
	}

	// Construct empty item
	ItemImpl(PayloadType type, const TagsMatcher &tagsMatcher, const FieldsSet &pkFields, std::shared_ptr<const Schema> schema,
			 ItemImplRawData &&rawData)
		: ItemImplRawData(std::move(rawData)),
		  payloadType_(type),
		  tagsMatcher_(tagsMatcher),
		  pkFields_(pkFields),
		  schema_(std::move(schema)) {}

	ItemImpl(PayloadType type, PayloadValue v, const TagsMatcher &tagsMatcher, std::shared_ptr<const Schema> schema = {})
		: ItemImplRawData(std::move(v)), payloadType_(type), tagsMatcher_(tagsMatcher), schema_{std::move(schema)} {
		tagsMatcher_.clearUpdated();
	}

	ItemImpl(const ItemImpl &) = delete;
	ItemImpl(ItemImpl &&) = default;
	ItemImpl &operator=(ItemImpl &&) = default;
	ItemImpl &operator=(const ItemImpl &) = delete;

	void ModifyField(std::string_view jsonPath, const VariantArray &keys, IndexExpressionEvaluator ev, FieldModifyMode mode);
	void ModifyField(const IndexedTagsPath &tagsPath, const VariantArray &keys, FieldModifyMode mode);
	void SetField(int field, const VariantArray &krs);
	void SetField(std::string_view jsonPath, const VariantArray &keys, IndexExpressionEvaluator ev);
	void DropField(std::string_view jsonPath, IndexExpressionEvaluator ev);
	Variant GetField(int field);
	void GetField(int field, VariantArray &);
	FieldsSet PkFields() const { return pkFields_; }
	int NameTag(std::string_view name) const { return tagsMatcher_.name2tag(name); }

	VariantArray GetValueByJSONPath(std::string_view jsonPath);

	std::string_view GetJSON();
	Error FromJSON(std::string_view slice, char **endp = nullptr, bool pkOnly = false);
	Error FromCJSON(ItemImpl *other);

	std::string_view GetCJSON(bool withTagsMatcher = false);
	std::string_view GetCJSON(WrSerializer &ser, bool withTagsMatcher = false);
	Error FromCJSON(std::string_view slice, bool pkOnly = false);
	Error FromMsgPack(std::string_view sbuf, size_t &offset);
	Error FromProtobuf(std::string_view sbuf);
	Error GetMsgPack(WrSerializer &wrser);
	Error GetProtobuf(WrSerializer &wrser);

	PayloadType Type() { return payloadType_; }
	PayloadValue &Value() { return payloadValue_; }
	PayloadValue &RealValue() { return realValue_; }
	Payload GetPayload() { return Payload(payloadType_, payloadValue_); }
	ConstPayload GetConstPayload() { return ConstPayload(payloadType_, payloadValue_); }
	std::shared_ptr<const Schema> GetSchema() { return schema_; }

	TagsMatcher &tagsMatcher() { return tagsMatcher_; }

	void SetPrecepts(const vector<string> &precepts) {
		precepts_ = precepts;
		cjson_ = std::string_view();
	}
	const vector<string> &GetPrecepts() { return precepts_; }
	void Unsafe(bool enable) { unsafe_ = enable; }
	bool IsUnsafe() const noexcept { return unsafe_; }
	void Clear() {
		tagsMatcher_ = TagsMatcher();
		precepts_.clear();
		cjson_ = std::string_view();
		holder_.reset();
		keyStringsHolder_.reset();
		sourceData_.reset();
		tupleData_.reset();
		ser_ = WrSerializer();

		GetPayload().Reset();
		payloadValue_.SetLSN(-1);

		unsafe_ = false;
		ns_.reset();
		realValue_.Free();
	}
	void SetNamespace(std::shared_ptr<Namespace> ns) { ns_ = std::move(ns); }
	std::shared_ptr<Namespace> GetNamespace() { return ns_; }

protected:
	// Index fields payload data
	PayloadType payloadType_;
	PayloadValue realValue_;
	TagsMatcher tagsMatcher_;
	FieldsSet pkFields_;
	std::shared_ptr<const Schema> schema_;

	WrSerializer ser_;

	bool unsafe_ = false;
	std::string_view cjson_;
	std::shared_ptr<Namespace> ns_;
	std::unique_ptr<MsgPackDecoder> msgPackDecoder_;
};

}  // namespace reindexer
