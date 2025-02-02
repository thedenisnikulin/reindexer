#pragma once

#include "core/ft/config/ftfastconfig.h"
#include "core/ft/ft_fast/dataholder.h"
#include "core/ft/ft_fast/dataprocessor.h"
#include "core/ft/typos.h"
#include "core/selectfunc/ctx/ftctx.h"
#include "indextext.h"

namespace reindexer {

template <typename T>
class FastIndexText : public IndexText<T> {
public:
	FastIndexText(const FastIndexText& other) : IndexText<T>(other) {
		initConfig(other.GetConfig());
		for (auto& idx : this->idx_map) idx.second.VDocID() = FtKeyEntryData::ndoc;
		this->CommitFulltext();
	}

	FastIndexText(const IndexDef& idef, PayloadType payloadType, const FieldsSet& fields)
		: IndexText<T>(idef, std::move(payloadType), fields) {
		initConfig();
	}
	std::unique_ptr<Index> Clone() override;
	IdSet::Ptr Select(FtCtx::Ptr fctx, FtDSLQuery& dsl, bool inTransaction, const RdxContext&) override final;
	IndexMemStat GetMemStat() override;
	Variant Upsert(const Variant& key, IdType id, bool& clearCache) override final;
	void Delete(const Variant& key, IdType id, StringsHolder&, bool& clearCache) override final;
	void SetOpts(const IndexOpts& opts) override final;

protected:
	void commitFulltextImpl() override final;
	FtFastConfig* GetConfig() const;
	void initConfig(const FtFastConfig* = nullptr);
	void initHolder(FtFastConfig&);

	template <class Data>
	void BuildVdocs(Data& data);
	std::unique_ptr<IDataHolder> holder_;
};

std::unique_ptr<Index> FastIndexText_New(const IndexDef& idef, PayloadType payloadType, const FieldsSet& fields);

}  // namespace reindexer
