/**
 * This file is part of the CernVM File System.
 */
#include "cvmfs_config.h"
#include "cache_tiered.h"

#include <errno.h>

#include <string>
#include <vector>

#include "logging.h"
#include "platform.h"
#include "quota.h"


std::string TieredCacheManager::Describe() {
  return "Tiered Cache\n"
    "  - upper layer: " + upper_->Describe() +
    "  - lower layer: " + lower_->Describe();
}


bool TieredCacheManager::DoFreeState(void *data) {
  SavedState *state = reinterpret_cast<SavedState *>(data);
  upper_->FreeState(-1, state->state_upper);
  lower_->FreeState(-1, state->state_lower);
  delete state;
  return true;
}


bool TieredCacheManager::DoRestoreState(void *data) {
  SavedState *state = reinterpret_cast<SavedState *>(data);
  upper_->RestoreState(-1, state->state_upper);
  lower_->RestoreState(-1, state->state_lower);
  return true;
}


void *TieredCacheManager::DoSaveState() {
  SavedState *state = new SavedState();
  state->state_upper = upper_->SaveState(-1);
  state->state_lower = lower_->SaveState(-1);
  return state;
}


int TieredCacheManager::Open(const BlessedObject &object) {
  int fd = upper_->Open(object);
  if ((fd >= 0) || (fd != -ENOENT)) {return fd;}

  int fd2 = lower_->Open(object);
  if (fd2 < 0) {return fd;}  // NOTE: use error code from upper.

  // Lower cache hit; upper cache miss.  Copy object into the upper cache.
  int64_t size = lower_->GetSize(fd2);
  if (size < 0) {
    lower_->Close(fd2);
    return fd;
  }

  void *txn = alloca(upper_->SizeOfTxn());
  if (upper_->StartTxn(object.id, size, txn) < 0) {
    lower_->Close(fd2);
    return fd;
  }
  upper_->CtrlTxn(object.info, 0, txn);

  std::vector<char> m_buffer;
  m_buffer.reserve(kCopyBufferSize);
  uint64_t remaining = size;
  uint64_t offset = 0;
  while (remaining > 0) {
    unsigned nbytes = remaining > kCopyBufferSize ? kCopyBufferSize : remaining;
    int64_t result = lower_->Pread(fd2, &m_buffer[0], nbytes, offset);
    // The file we are reading is supposed to be exactly `size` bytes.
    if ((result < 0) || (result != nbytes)) {
      lower_->Close(fd2);
      upper_->AbortTxn(txn);
      return fd;
    }
    result = upper_->Write(&m_buffer[0], nbytes, txn);
    if (result < 0) {
      lower_->Close(fd2);
      upper_->AbortTxn(txn);
      return fd;
    }
    offset += nbytes;
    remaining -= nbytes;
  }
  lower_->Close(fd2);
  int fd_return = upper_->OpenFromTxn(txn);
  if (fd_return < 0) {
    upper_->AbortTxn(txn);
    return fd;
  }
  if (upper_->CommitTxn(txn) < 0) {
    upper_->Close(fd_return);
    return fd;
  }
  return fd_return;
}


int TieredCacheManager::StartTxn(const shash::Any &id, uint64_t size, void *txn)
{
  int upper_result = upper_->StartTxn(id, size, txn);
  if (lower_readonly_ || (upper_result < 0)) {
    return upper_result;
  }

  void *txn2 = static_cast<char *>(txn) + upper_->SizeOfTxn();
  int lower_result = lower_->StartTxn(id, size, txn2);
  if (lower_result < 0) {
    upper_->AbortTxn(txn);
  }
  return lower_result;
}


CacheManager *TieredCacheManager::Create(
  CacheManager *upper_cache,
  CacheManager *lower_cache)
{
  TieredCacheManager *cache_mgr =
    new TieredCacheManager(upper_cache, lower_cache);
  delete cache_mgr->quota_mgr_;
  cache_mgr->quota_mgr_ = upper_cache->quota_mgr();
  return cache_mgr;
}


void TieredCacheManager::CtrlTxn(
  const ObjectInfo &object_info,
  const int flags,
  void *txn)
{
  upper_->CtrlTxn(object_info, flags, txn);
  if (!lower_readonly_) {
    void *txn2 = static_cast<char*>(txn) + upper_->SizeOfTxn();
    lower_->CtrlTxn(object_info, flags, txn2);
  }
}


int64_t TieredCacheManager::Write(const void *buf, uint64_t size, void *txn) {
  int upper_result = upper_->Write(buf, size, txn);
  if (lower_readonly_ || (upper_result < 0)) { return upper_result; }

  void *txn2 = static_cast<char*>(txn) + upper_->SizeOfTxn();
  return lower_->Write(buf, size, txn2);
}


int TieredCacheManager::Reset(void *txn) {
  int upper_result = upper_->Reset(txn);

  int lower_result = upper_result;
  if (!lower_readonly_) {
    void *txn2 = static_cast<char*>(txn) + upper_->SizeOfTxn();
    lower_result = lower_->Reset(txn2);
  }

  return (upper_result < 0) ? upper_result : lower_result;
}


int TieredCacheManager::AbortTxn(void *txn) {
  int upper_result = upper_->AbortTxn(txn);

  int lower_result = upper_result;
  if (!lower_readonly_) {
    void *txn2 = static_cast<char*>(txn) + upper_->SizeOfTxn();
    lower_result = lower_->AbortTxn(txn2);
  }

  return (upper_result < 0) ? upper_result : lower_result;
}


int TieredCacheManager::CommitTxn(void *txn) {
  int upper_result = upper_->CommitTxn(txn);

  int lower_result = upper_result;
  if (!lower_readonly_) {
    void *txn2 = static_cast<char*>(txn) + upper_->SizeOfTxn();
    lower_result = lower_->CommitTxn(txn2);
  }

  // Transaction is successful as long as the storage was successful
  // in the upper cache (which may have already handed out a FD via
  // OpenFromTxn).
  //
  // If there wasn't the OpenFromTxn semantics, we could have this
  // call succeed if *either* transaction was successful.
  if (!upper_result && lower_result) {
    LogCvmfs(kLogCache, kLogSyslogWarn | kLogDebug, "Commit of transaction "
             "failed in lower cache but succeeded in upper cache.");
  }
  return upper_result;
}


void TieredCacheManager::Spawn() {
  upper_->Spawn();
  lower_->Spawn();
}


TieredCacheManager::~TieredCacheManager() {
  quota_mgr_ = NULL;  // gets deleted by upper
  delete upper_;
  delete lower_;
}
