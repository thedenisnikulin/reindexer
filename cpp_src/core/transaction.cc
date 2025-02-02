#include "transaction.h"
#include "transactionimpl.h"
namespace reindexer {

Transaction::Transaction(const string &nsName, const PayloadType &pt, const TagsMatcher &tm, const FieldsSet &pf,
						 std::shared_ptr<const Schema> schema)
	: impl_(new TransactionImpl(nsName, pt, tm, pf, schema)) {}

Transaction::Transaction(const Error &err) : status_(err) {}

Transaction::~Transaction() = default;
Transaction::Transaction(Transaction &&) noexcept = default;
Transaction &Transaction::operator=(Transaction &&) noexcept = default;

const string &Transaction::GetName() {
	static std::string empty;
	if (impl_)
		return impl_->nsName_;
	else
		return empty;
}

void Transaction::Insert(Item &&item) {
	if (impl_) impl_->Insert(move(item));
}
void Transaction::Update(Item &&item) {
	if (impl_) impl_->Update(move(item));
}
void Transaction::Upsert(Item &&item) {
	if (impl_) impl_->Upsert(move(item));
}
void Transaction::Delete(Item &&item) {
	if (impl_) impl_->Delete(move(item));
}
void Transaction::Modify(Item &&item, ItemModifyMode mode) {
	if (impl_) impl_->Modify(move(item), mode);
}

void Transaction::Modify(Query &&query) {
	if (impl_) impl_->Modify(move(query));
}

Item Transaction::NewItem() { return impl_->NewItem(); }

vector<TransactionStep> &Transaction::GetSteps() {
	assertrx(impl_);
	return impl_->steps_;
}

const vector<TransactionStep> &Transaction::GetSteps() const {
	assertrx(impl_);
	return impl_->steps_;
}

Item Transaction::GetItem(TransactionStep &&st) {
	assertrx(impl_);
	return impl_->GetItem(std::move(st));
}

bool Transaction::IsTagsUpdated() const {
	assertrx(impl_);
	return impl_->tagsUpdated_;
}

Transaction::time_point Transaction::GetStartTime() const {
	assertrx(impl_);
	return impl_->startTime_;
}

}  // namespace reindexer
