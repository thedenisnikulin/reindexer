#include "namespace.h"
#include "core/storage/storagefactory.h"
#include "tools/flagguard.h"
#include "tools/fsops.h"
#include "tools/logger.h"

namespace reindexer {

#define handleInvalidation(Fn) nsFuncWrapper<decltype(&Fn), &Fn>

void Namespace::CommitTransaction(Transaction& tx, QueryResults& result, const RdxContext& ctx) {
	auto ns = atomicLoadMainNs();
	bool enablePerfCounters = ns->enablePerfCounters_.load(std::memory_order_relaxed);
	if (enablePerfCounters) {
		txStatsCounter_.Count(tx);
	}
	PerfStatCalculatorMT txCommitCalc(commitStatsCounter_, enablePerfCounters);
	if (needNamespaceCopy(ns, tx)) {
		PerfStatCalculatorMT calc(ns->updatePerfCounter_, enablePerfCounters);
		contexted_unique_lock<Mutex, const RdxContext> lck(clonerMtx_, &ctx);
		ns = ns_;
		if (needNamespaceCopy(ns, tx)) {
			PerfStatCalculatorMT nsCopyCalc(copyStatsCounter_, enablePerfCounters);
			calc.SetCounter(ns->updatePerfCounter_);
			calc.LockHit();
			logPrintf(LogTrace, "Namespace::CommitTransaction creating copy for (%s)", ns->name_);
			hasCopy_.store(true, std::memory_order_release);
			CounterGuardAIR32 cg(ns->cancelCommitCnt_);
			try {
				auto rlck = ns->rLock(ctx);
				auto storageLock = ns->storage_.FullLock();
				cg.Reset();
				nsCopy_.reset(new NamespaceImpl(*ns, storageLock));
				nsCopyCalc.HitManualy();
				nsCopy_->CommitTransaction(tx, result, NsContext(ctx).NoLock());
				if (nsCopy_->lastUpdateTime_) {
					nsCopy_->lastUpdateTime_ -= nsCopy_->config_.optimizationTimeout * 2;
					nsCopy_->optimizeIndexes(NsContext(ctx).NoLock());
					nsCopy_->warmupFtIndexes();
				}
				try {
					nsCopy_->storage_.InheritUpdatesFrom(ns->storage_,
														 storageLock);	// Updates can not be flushed until tx is commited into ns copy
				} catch (Error& e) {
					// This exception should never be seen - there are no good ways to recover from it
					assertf(false, "Error during storage moving in namespace (%s) copying: %s", ns->name_, e.what());
				}

				calc.SetCounter(nsCopy_->updatePerfCounter_);
				ns->markReadOnly();
				ns->wal_.SetStorage(std::weak_ptr<datastorage::IDataStorage>(), true);
				atomicStoreMainNs(nsCopy_.release());
				hasCopy_.store(false, std::memory_order_release);
			} catch (...) {
				calc.enable_ = false;
				nsCopy_.reset();
				hasCopy_.store(false, std::memory_order_release);
				throw;
			}
			ns = ns_;
			lck.unlock();
			ns->storage_.TryForceFlush();
			return;
		}
	}
	handleInvalidation(NamespaceImpl::CommitTransaction)(tx, result, NsContext(ctx));
}

NamespacePerfStat Namespace::GetPerfStat(const RdxContext& ctx) {
	NamespacePerfStat stats = handleInvalidation(NamespaceImpl::GetPerfStat)(ctx);
	stats.transactions = txStatsCounter_.Get();
	auto copyStats = copyStatsCounter_.Get<PerfStat>();
	stats.transactions.totalCopyCount = copyStats.totalHitCount;
	stats.transactions.minCopyTimeUs = copyStats.minTimeUs;
	stats.transactions.maxCopyTimeUs = copyStats.maxTimeUs;
	stats.transactions.avgCopyTimeUs = copyStats.totalTimeUs / (copyStats.totalHitCount ? copyStats.totalHitCount : 1);
	auto commitStats = commitStatsCounter_.Get<PerfStat>();
	stats.transactions.totalCount = commitStats.totalHitCount;
	stats.transactions.minCommitTimeUs = commitStats.minTimeUs;
	stats.transactions.maxCommitTimeUs = commitStats.maxTimeUs;
	stats.transactions.avgCommitTimeUs = commitStats.totalTimeUs / (commitStats.totalHitCount ? commitStats.totalHitCount : 1);
	return stats;
}

bool Namespace::needNamespaceCopy(const NamespaceImpl::Ptr& ns, const Transaction& tx) const noexcept {
	auto stepsCount = tx.GetSteps().size();
	auto startCopyPolicyTxSize = static_cast<uint32_t>(startCopyPolicyTxSize_.load(std::memory_order_relaxed));
	auto copyPolicyMultiplier = static_cast<uint32_t>(copyPolicyMultiplier_.load(std::memory_order_relaxed));
	auto txSizeToAlwaysCopy = static_cast<uint32_t>(txSizeToAlwaysCopy_.load(std::memory_order_relaxed));
	return ((stepsCount >= startCopyPolicyTxSize) && (ns->GetItemsCapacity() <= copyPolicyMultiplier * stepsCount)) ||
		   (stepsCount >= txSizeToAlwaysCopy);
}

void Namespace::doRename(Namespace::Ptr dst, const std::string& newName, const std::string& storagePath, const RdxContext& ctx) {
	std::string dbpath;
	awaitMainNs(ctx)->storage_.Flush();
	auto lck = handleInvalidation(NamespaceImpl::wLock)(ctx);
	auto& srcNs = *atomicLoadMainNs();	// -V758
	NamespaceImpl::Mutex* dstMtx = nullptr;
	NamespaceImpl::Ptr dstNs;
	if (dst) {
		while (true) {
			try {
				dstNs = dst->awaitMainNs(ctx);
				dstMtx = dstNs->wLock(ctx).release();
				break;
			} catch (const Error& e) {
				if (e.code() != errNamespaceInvalidated) {
					throw;
				} else {
					std::this_thread::yield();
				}
			}
		}
		dbpath = dstNs->storage_.Path();
	} else if (newName == srcNs.name_) {
		return;
	}

	if (dbpath.empty()) {
		dbpath = fs::JoinPath(storagePath, newName);
	} else {
		dstNs->storage_.Destroy();
	}

	const bool hadStorage = (srcNs.storage_.IsValid());
	auto storageType = StorageType::LevelDB;
	const auto srcDbpath = srcNs.storage_.Path();
	if (hadStorage) {
		storageType = srcNs.storage_.Type();
		srcNs.storage_.Close();
		fs::RmDirAll(dbpath);
		int renameRes = fs::Rename(srcDbpath, dbpath);
		if (renameRes < 0) {
			if (dst) {
				assertrx(dstMtx);
				dstMtx->unlock();
			}
			throw Error(errParams, "Unable to rename '%s' to '%s'", srcDbpath, dbpath);
		}
	}

	if (dst) {
		logPrintf(LogInfo, "Rename namespace '%s' to '%s'", srcNs.name_, dstNs->name_);
		srcNs.name_ = dstNs->name_;
		assertrx(dstMtx);
		dstMtx->unlock();
	} else {
		logPrintf(LogInfo, "Rename namespace '%s' to '%s'", srcNs.name_, newName);
		srcNs.name_ = newName;
	}
	srcNs.payloadType_.SetName(srcNs.name_);

	if (hadStorage) {
		logPrintf(LogTrace, "Storage was moved from %s to %s", srcDbpath, dbpath);
		auto status = srcNs.storage_.Open(storageType, srcNs.name_, dbpath, srcNs.storageOpts_);
		if (!status.ok()) {
			srcNs.storage_.Close();
			throw status;
		}
		srcNs.wal_.SetStorage(srcNs.storage_.GetStoragePtr(), false);
	}
	if (srcNs.repl_.temporary) {
		srcNs.repl_.temporary = false;
		srcNs.saveReplStateToStorage();
	}
}

}  // namespace reindexer
