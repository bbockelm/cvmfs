/**
 * CernVM-FS is a FUSE module which implements an HTTP read-only filesystem.
 * The original idea is based on GROW-FS.
 *
 * CernVM-FS shows a remote HTTP directory as local file system.  The client
 * sees all available files.  On first access, a file is downloaded and
 * cached locally.  All downloaded pieces are verified with SHA1.
 *
 * To do so, a directory hive has to be transformed into a CVMFS2
 * "repository".  This can be done by the CernVM-FS server tools.
 *
 * This preparation of directories is transparent to web servers and
 * web proxies.  They just serve static content, i.e. arbitrary files.
 * Any HTTP server should do the job.  We use Apache + Squid.  Serving
 * files from the memory of a web proxy brings a significant performance
 * improvement.
 */
// TODO: ndownload into cache

#define ENOATTR ENODATA  /**< instead of including attr/xattr.h */
#define FUSE_USE_VERSION 26
#define __STDC_FORMAT_MACROS

#include "cvmfs_config.h"

#include <dirent.h>
#include <errno.h>
#include <stddef.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifndef __APPLE__
#include <sys/statfs.h>
#endif
#include <sys/wait.h>
#include <sys/errno.h>
#include <sys/mount.h>
#include <sys/file.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pthread.h>
#include <sys/xattr.h>
#include <inttypes.h>

#include <openssl/crypto.h>
#include <fuse/fuse_lowlevel.h>
#include <fuse/fuse_opt.h>
#include <google/dense_hash_map>

#include <cstdlib>
#include <cstring>
#include <csignal>
#include <ctime>
#include <cassert>
#include <cstdio>

#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <functional>

#include "cvmfs.h"

#include "platform.h"
#include "logging.h"
#include "tracer.h"
#include "download.h"
#include "cache.h"
#include "nfs_maps.h"
#include "hash.h"
#include "talk.h"
#include "monitor.h"
#include "signature.h"
#include "quota.h"
#include "util.h"
#include "util_concurrency.h"
#include "atomic.h"
#include "lru.h"
#include "peers.h"
#include "directory_entry.h"
#include "file_chunk.h"
#include "compression.h"
#include "duplex_sqlite3.h"
#include "shortstring.h"
#include "smalloc.h"
#include "globals.h"
#include "options.h"
#include "loader.h"
#include "glue_buffer.h"
#include "compat.h"
#include "history.h"
#include "manifest_fetch.h"

#ifdef FUSE_CAP_EXPORT_SUPPORT
#define CVMFS_NFS_SUPPORT
#else
#warning "No NFS support, Fuse too old"
#endif

using namespace std;  // NOLINT

namespace cvmfs {

const char *kDefaultCachedir = "/var/lib/cvmfs/default";
const unsigned kDefaultTimeout = 2;
const double kDefaultKCacheTimeout = 60.0;
const unsigned kReloadSafetyMargin = 500;  // in milliseconds
const unsigned kDefaultNumConnections = 16;
const uint64_t kDefaultMemcache = 16*1024*1024;  // 16M RAM for meta-data caches
const uint64_t kDefaultCacheSizeMb = 1024*1024*1024;  // 1G
const unsigned int kShortTermTTL = 180;  /**< If catalog reload fails, try again
                                              in 3 minutes */
const time_t kIndefiniteDeadline = time_t(-1);

const int kMaxInitIoDelay = 32; /**< Maximum start value for exponential
                                     backoff */
const int kMaxIoDelay = 2000; /**< Maximum 2 seconds */
const int kForgetDos = 10000; /**< Clear DoS memory after 10 seconds */

/**
 * Prevent DoS attacks on the Squid server
 */
static struct {
  time_t timestamp;
  int delay;
} previous_io_error_;


/**
 * Stores the initial catalog revision (in order to detect overflows) and
 * the incarnation (number of reloads) of the Fuse module
 */
struct InodeGenerationInfo {
  InodeGenerationInfo() {
    version = 2;
    initial_revision = 0;
    incarnation = 0;
    overflow_counter = 0;
    inode_generation = 0;
  }
  unsigned version;
  uint64_t initial_revision;
  uint32_t incarnation;
  uint32_t overflow_counter;  // not used any more
  uint64_t inode_generation;
};
InodeGenerationInfo inode_generation_info_;

/**
 * For cvmfs_opendir / cvmfs_readdir
 * TODO: use mmap for very large listings
 */
struct DirectoryListing {
  char *buffer;  /**< Filled by fuse_add_direntry */

  // Not really used anymore.  But directory listing needs to be migrated during
  // hotpatch. If buffer is allocated by smmap, capacity is zero.
  size_t size;
  size_t capacity;

  DirectoryListing() : buffer(NULL), size(0), capacity(0) { }
};

const loader::LoaderExports *loader_exports_ = NULL;
bool foreground_ = false;
bool nfs_maps_ = false;
string *mountpoint_ = NULL;
string *cachedir_ = NULL;
string *nfs_shared_dir_ = NULL;
string *tracefile_ = NULL;
string *repository_name_ = NULL;  /**< Expected repository name,
                                       e.g. atlas.cern.ch */
string *repository_tag_ = NULL;
pid_t pid_ = 0;  /**< will be set after deamon() */
time_t boot_time_;
unsigned max_ttl_ = 0;
pthread_mutex_t lock_max_ttl_ = PTHREAD_MUTEX_INITIALIZER;
catalog::InodeGenerationAnnotation *inode_annotation_ = NULL;
cache::CatalogManager *catalog_manager_ = NULL;
lru::InodeCache *inode_cache_ = NULL;
lru::PathCache *path_cache_ = NULL;
lru::Md5PathCache *md5path_cache_ = NULL;
glue::InodeTracker *inode_tracker_ = NULL;

double kcache_timeout_ = kDefaultKCacheTimeout;
bool fixed_catalog_ = false;

/**
 * in maintenance mode, cache timeout is 0 and catalogs are not reloaded
 */
atomic_int32 maintenance_mode_;
atomic_int32 catalogs_expired_;
atomic_int32 drainout_mode_;
atomic_int32 reload_critical_section_;
time_t drainout_deadline_;
time_t catalogs_valid_until_;

typedef google::dense_hash_map<uint64_t, DirectoryListing,
                               hash_murmur<uint64_t> >
        DirectoryHandles;
DirectoryHandles *directory_handles_ = NULL;
pthread_mutex_t lock_directory_handles_ = PTHREAD_MUTEX_INITIALIZER;
uint64_t next_directory_handle_ = 0;

// contains inode to chunklist and handle to fd maps
ChunkTables *chunk_tables_;

atomic_int64 num_fs_open_;
atomic_int64 num_fs_dir_open_;
atomic_int64 num_fs_lookup_;
atomic_int64 num_fs_lookup_negative_;
atomic_int64 num_fs_stat_;
atomic_int64 num_fs_read_;
atomic_int64 num_fs_readlink_;
atomic_int64 num_fs_forget_;
atomic_int32 num_io_error_;
atomic_int32 open_files_; /**< number of currently open files by Fuse calls */
atomic_int32 open_dirs_; /**< number of currently open directories */
unsigned max_open_files_; /**< maximum allowed number of open files */
const int kNumReservedFd = 512;  /**< Number of reserved file descriptors for
                                      internal use */

/**
 * Ensures that within a callback all operations take place on the same
 * catalog revision.
 */
class RemountFence : public SingleCopy {
 public:
  RemountFence() {
    atomic_init64(&counter_);
    atomic_init32(&blocking_);
  }
  void Enter() {
    while (atomic_read32(&blocking_)) {
      SafeSleepMs(100);
    }
    atomic_inc64(&counter_);
  }
  void Leave() {
    atomic_dec64(&counter_);
  }
  void Block() {
    atomic_cas32(&blocking_, 0, 1);
    while (atomic_read64(&counter_) > 0) {
      SafeSleepMs(100);
    }
  }
  void Unblock() {
    atomic_cas32(&blocking_, 1, 0);
  }
 private:
  atomic_int64 counter_;
  atomic_int32 blocking_;
};
RemountFence *remount_fence_;


unsigned GetMaxTTL() {
  pthread_mutex_lock(&lock_max_ttl_);
  const unsigned current_max = max_ttl_/60;
  pthread_mutex_unlock(&lock_max_ttl_);

  return current_max;
}


void SetMaxTTL(const unsigned value) {
  pthread_mutex_lock(&lock_max_ttl_);
  max_ttl_ = value*60;
  pthread_mutex_unlock(&lock_max_ttl_);
}


static unsigned GetEffectiveTTL() {
  const unsigned max_ttl = GetMaxTTL()*60;
  const unsigned catalog_ttl = catalog_manager_->GetTTL();

  return max_ttl ? std::min(max_ttl, catalog_ttl) : catalog_ttl;
}


static inline double GetKcacheTimeout() {
  if (atomic_read32(&drainout_mode_) || atomic_read32(&maintenance_mode_))
    return 0.0;
  return kcache_timeout_;
}


void GetReloadStatus(bool *drainout_mode, bool *maintenance_mode) {
  *drainout_mode = atomic_read32(&drainout_mode_);
  *maintenance_mode = atomic_read32(&maintenance_mode_);
}


unsigned GetRevision() {
  return catalog_manager_->GetRevision();
};


std::string GetOpenCatalogs() {
  return catalog_manager_->PrintHierarchy();
}


void ResetErrorCounters() {
  atomic_init32(&num_io_error_);
}


static bool UseWatchdog() {
  if (loader_exports_ == NULL || loader_exports_->version < 2) {
    return true; // spawn watchdog by default
                 // Note: with library versions before 2.1.8 it might not create
                 //       stack traces properly in all cases
  }

  return ! loader_exports_->disable_watchdog;
}


void GetLruStatistics(lru::Statistics *inode_stats, lru::Statistics *path_stats,
                      lru::Statistics *md5path_stats)
{
  *inode_stats = inode_cache_->statistics();
  *path_stats = path_cache_->statistics();
  *md5path_stats = md5path_cache_->statistics();
}


string PrintInodeTrackerStatistics() {
  return inode_tracker_->GetStatistics().Print() + "\n";
}


std::string PrintInodeGeneration() {
  return "init-catalog-revision: " +
    StringifyInt(inode_generation_info_.initial_revision) + "  " +
    "current-catalog-revision: " +
    StringifyInt(catalog_manager_->GetRevision()) + "  " +
    "incarnation: " + StringifyInt(inode_generation_info_.incarnation) + "  " +
    "inode generation: " + StringifyInt(inode_generation_info_.inode_generation)
    + "\n";
}


catalog::Statistics GetCatalogStatistics() {
  return catalog_manager_->statistics();
}

string GetCertificateStats() {
  return catalog_manager_->GetCertificateStats();
}

string GetFsStats() {
  return "lookup(all): " + StringifyInt(atomic_read64(&num_fs_lookup_)) + "  " +
    "lookup(negative): " + StringifyInt(atomic_read64(&num_fs_lookup_negative_))
      + "  " +
    "stat(): " + StringifyInt(atomic_read64(&num_fs_stat_)) + "  " +
    "open(): " + StringifyInt(atomic_read64(&num_fs_open_)) + "  " +
    "diropen(): " + StringifyInt(atomic_read64(&num_fs_dir_open_)) + "  " +
    "read(): " + StringifyInt(atomic_read64(&num_fs_read_)) + "  " +
    "readlink(): " + StringifyInt(atomic_read64(&num_fs_readlink_)) + "  " +
    "forget(): " + StringifyInt(atomic_read64(&num_fs_forget_)) + "\n";
}


static void AlarmReload(int signal __attribute__((unused)),
                        siginfo_t *siginfo __attribute__((unused)),
                        void *context __attribute__((unused)))
{
  atomic_cas32(&catalogs_expired_, 0, 1);
}


/**
 * If there is a new catalog version, switches to drainout mode.
 * lookup or getattr will take care of actual remounting once the caches are
 * drained out.
 */
catalog::LoadError RemountStart() {
  catalog::LoadError retval = catalog_manager_->Remount(true);
  if (retval == catalog::kLoadNew) {
    LogCvmfs(kLogCvmfs, kLogDebug,
             "new catalog revision available, draining out meta-data caches");
    unsigned safety_margin = kReloadSafetyMargin/1000;
    if (safety_margin == 0)
      safety_margin = 1;
    drainout_deadline_ = time(NULL) + int(kcache_timeout_) + safety_margin;
    atomic_cas32(&drainout_mode_, 0, 1);
  }
  return retval;
}


/**
 * If the caches are drained out, a new catalog revision is applied and
 * kernel caches are activated again.
 */
static void RemountFinish() {
  if (!atomic_cas32(&reload_critical_section_, 0, 1))
    return;
  if (!atomic_read32(&drainout_mode_)) {
    atomic_cas32(&reload_critical_section_, 1, 0);
    return;
  }

  if (time(NULL) > drainout_deadline_) {
    LogCvmfs(kLogCvmfs, kLogDebug, "caches drained out, applying new catalog");

    // No new inserts into caches
    inode_cache_->Pause();
    path_cache_->Pause();
    md5path_cache_->Pause();
    inode_cache_->Drop();
    path_cache_->Drop();
    md5path_cache_->Drop();

    // Ensure that all Fuse callbacks left the catalog query code
    remount_fence_->Block();
    catalog::LoadError retval = catalog_manager_->Remount(false);
    if (inode_annotation_) {
      inode_generation_info_.inode_generation =
        inode_annotation_->GetGeneration();
    }
    remount_fence_->Unblock();

    inode_cache_->Resume();
    path_cache_->Resume();
    md5path_cache_->Resume();

    atomic_cas32(&drainout_mode_, 1, 0);
    if ((retval == catalog::kLoadFail) || (retval == catalog::kLoadNoSpace) ||
        catalog_manager_->offline_mode())
    {
      LogCvmfs(kLogCvmfs, kLogDebug, "reload/finish failed, "
               "applying short term TTL");
      alarm(kShortTermTTL);
      catalogs_valid_until_ = time(NULL) + kShortTermTTL;
    } else {
      LogCvmfs(kLogCvmfs, kLogSyslog, "switched to catalog revision %d",
               catalog_manager_->GetRevision());
      alarm(GetEffectiveTTL());
      catalogs_valid_until_ = time(NULL) + GetEffectiveTTL();
    }
  }

  atomic_cas32(&reload_critical_section_, 1, 0);
}


/**
 * Runs at the beginning of lookup, checks if a previously started remount needs
 * to be finished or starts a new remount if the TTL timer has been fired.
 */
static void RemountCheck() {
  if (atomic_read32(&maintenance_mode_) == 1)
    return;
  RemountFinish();

  if (atomic_cas32(&catalogs_expired_, 1, 0)) {
    LogCvmfs(kLogCvmfs, kLogDebug, "catalog TTL expired, reload");
    catalog::LoadError retval = RemountStart();
    if ((retval == catalog::kLoadFail) || (retval == catalog::kLoadNoSpace)) {
      LogCvmfs(kLogCvmfs, kLogDebug, "reload failed, applying short term TTL");
      alarm(kShortTermTTL);
      catalogs_valid_until_ = time(NULL) + kShortTermTTL;
    } else if (retval == catalog::kLoadUp2Date) {
      LogCvmfs(kLogCvmfs, kLogDebug,
               "catalog up to date, applying effective TTL");
      alarm(GetEffectiveTTL());
      catalogs_valid_until_ = time(NULL) + GetEffectiveTTL();
    }
  }
}


static bool GetDirentForInode(const fuse_ino_t ino,
                              catalog::DirectoryEntry *dirent)
{
  // Lookup inode in cache
  if (inode_cache_->Lookup(ino, dirent))
    return true;

  // Lookup inode in catalog
  if (nfs_maps_) {
    // NFS mode
    PathString path;
    if (nfs_maps::GetPath(ino, &path) &&
        catalog_manager_->LookupPath(path, catalog::kLookupSole, dirent))
    {
      // Fix inodes
      dirent->set_inode(ino);
      inode_cache_->Insert(ino, *dirent);
      return true;
    }
  } else {
    // Normal mode
    PathString path;
    if (ino == catalog_manager_->GetRootInode()) {
      catalog_manager_->LookupPath(PathString(), catalog::kLookupSole, dirent);
      dirent->set_inode(ino);
      inode_cache_->Insert(ino, *dirent);
      return true;
    }
    if (inode_tracker_->FindPath(ino, &path) &&
        catalog_manager_->LookupPath(path, catalog::kLookupSole, dirent))
    {
      // Fix inodes
      dirent->set_inode(ino);
      inode_cache_->Insert(ino, *dirent);
      return true;
    }
  }

  // Can happen after reload of catalogs
  LogCvmfs(kLogCvmfs, kLogDebug, "GetDirentForInode lookup failure");
  return false;
}


static bool GetDirentForPath(const PathString &path,
                             catalog::DirectoryEntry *dirent)
{
  uint64_t live_inode = 0;
  if (!nfs_maps_)
    live_inode = inode_tracker_->FindInode(path);

  hash::Md5 md5path(path.GetChars(), path.GetLength());
  if (md5path_cache_->Lookup(md5path, dirent)) {
    if (dirent->GetSpecial() == catalog::kDirentNegative)
      return false;
    if (!nfs_maps_ && (live_inode != 0))
      dirent->set_inode(live_inode);
    return true;
  }

  // Lookup inode in catalog TODO: not twice md5 calculation
  bool retval;
  retval = catalog_manager_->LookupPath(path, catalog::kLookupSole, dirent);
  if (retval) {
    if (nfs_maps_) {
      // Fix inode
      dirent->set_inode(nfs_maps::GetInode(path));
    } else {
      // TODO: Ensure that regular files get a new inode in order to avoid
      // page cache mixup
      if (live_inode != 0)
        dirent->set_inode(live_inode);
    }
    md5path_cache_->Insert(md5path, *dirent);
    return true;
  }

  LogCvmfs(kLogCvmfs, kLogDebug, "GetDirentForPath, no entry");
  md5path_cache_->InsertNegative(md5path);
  return false;
}


static bool GetPathForInode(const fuse_ino_t ino, PathString *path) {
  // Check the path cache first
  if (path_cache_->Lookup(ino, path))
    return true;

  if (nfs_maps_) {
    // NFS mode, just a lookup
    LogCvmfs(kLogCvmfs, kLogDebug, "MISS %d - lookup in NFS maps", ino);
    if (nfs_maps::GetPath(ino, path)) {
      path_cache_->Insert(ino, *path);
      return true;
    }
    return false;
  }

  if (ino == catalog_manager_->GetRootInode())
    return true;

  LogCvmfs(kLogCvmfs, kLogDebug, "MISS %d - looking in inode tracker", ino);
  bool retval = inode_tracker_->FindPath(ino, path);
  assert(retval);
  path_cache_->Insert(ino, *path);
  return true;
}


/**
 * Find the inode number of a file name in a directory given by inode.
 * This or getattr is called as kind of prerequisit to every operation.
 * We do check catalog TTL here (and reload, if necessary).
 */
static void cvmfs_lookup(fuse_req_t req, fuse_ino_t parent, const char *name) {
  atomic_inc64(&num_fs_lookup_);
  RemountCheck();

  remount_fence_->Enter();
  parent = catalog_manager_->MangleInode(parent);
  LogCvmfs(kLogCvmfs, kLogDebug,
           "cvmfs_lookup in parent inode: %"PRIu64" for name: %s", parent, name);

  PathString path;
  PathString parent_path;
  catalog::DirectoryEntry dirent;
  struct fuse_entry_param result;

  memset(&result, 0, sizeof(result));
  double timeout = GetKcacheTimeout();
  result.attr_timeout = timeout;
  result.entry_timeout = timeout;

  // Special NFS lookups
  if ((strcmp(name, ".") == 0) || (strcmp(name, "..") == 0)) {
    if (GetDirentForInode(parent, &dirent)) {
      if (strcmp(name, ".") == 0) {
        goto reply_positive;
      } else {
        if (dirent.inode() == catalog_manager_->GetRootInode()) {
          dirent.set_inode(1);
          goto reply_positive;
        }
        if (GetPathForInode(parent, &parent_path) &&
            GetDirentForPath(GetParentPath(parent_path), &dirent))
        {
          goto reply_positive;
        } else {
          goto reply_negative;
        }
      }
    } else {
      goto reply_negative;
    }
  }

  if (!GetPathForInode(parent, &parent_path)) {
    LogCvmfs(kLogCvmfs, kLogDebug, "no path for parent inode found");
    goto reply_negative;
  }

  path.Assign(parent_path);
  path.Append("/", 1);
  path.Append(name, strlen(name));
  tracer::Trace(tracer::kFuseLookup, path, "lookup()");
  if (!GetDirentForPath(path, &dirent)) {
    goto reply_negative;
  }

 reply_positive:
  if (!nfs_maps_)
    inode_tracker_->VfsGet(dirent.inode(), path);
  remount_fence_->Leave();
  result.ino = dirent.inode();
  result.attr = dirent.GetStatStructure();
  fuse_reply_entry(req, &result);
  return;

 reply_negative:
  remount_fence_->Leave();
  atomic_inc64(&num_fs_lookup_negative_);
  result.ino = 0;
  fuse_reply_entry(req, &result);
}


/**
 *
 */
static void cvmfs_forget(fuse_req_t req, fuse_ino_t ino, unsigned long nlookup)
{
  atomic_inc64(&cvmfs::num_fs_forget_);

  // The libfuse high-level library does the same
  if (ino == FUSE_ROOT_ID) {
    fuse_reply_none(req);
    return;
  }

  remount_fence_->Enter();
  ino = catalog_manager_->MangleInode(ino);
  LogCvmfs(kLogCvmfs, kLogDebug, "forget on inode %"PRIu64" by %u",
           ino, nlookup);
  if (!nfs_maps_)
    inode_tracker_->VfsPut(ino, nlookup);
  remount_fence_->Leave();
  fuse_reply_none(req);
}


/**
 * Transform a cvmfs dirent into a struct stat.
 */
static void cvmfs_getattr(fuse_req_t req, fuse_ino_t ino,
                          struct fuse_file_info *fi)
{
  atomic_inc64(&num_fs_stat_);
  RemountCheck();

  remount_fence_->Enter();
  ino = catalog_manager_->MangleInode(ino);
  LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_getattr (stat) for inode: %"PRIu64, ino);

  catalog::DirectoryEntry dirent;
  const bool found = GetDirentForInode(ino, &dirent);
  remount_fence_->Leave();

  if (!found) {
    fuse_reply_err(req, ENOENT);
    return;
  }

  struct stat info = dirent.GetStatStructure();

  fuse_reply_attr(req, &info, GetKcacheTimeout());
}


/**
 * Reads a symlink from the catalog.  Environment variables are expanded.
 */
static void cvmfs_readlink(fuse_req_t req, fuse_ino_t ino) {
  atomic_inc64(&num_fs_readlink_);

  remount_fence_->Enter();
  ino = catalog_manager_->MangleInode(ino);
  LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_readlink on inode: %"PRIu64, ino);

  catalog::DirectoryEntry dirent;
  const bool found = GetDirentForInode(ino, &dirent);
  remount_fence_->Leave();

  if (!found) {
    fuse_reply_err(req, ENOENT);
    return;
  }

  if (!dirent.IsLink()) {
    fuse_reply_err(req, EINVAL);
    return;
  }

  fuse_reply_readlink(req, dirent.symlink().c_str());
}


static void AddToDirListing(const fuse_req_t req,
                            const char *name, const struct stat *stat_info,
                            BigVector<char> *listing)
{
  LogCvmfs(kLogCvmfs, kLogDebug, "Add to listing: %s, inode %"PRIu64,
           name, stat_info->st_ino);
  size_t remaining_size = listing->capacity() - listing->size();
  const size_t entry_size = fuse_add_direntry(req, NULL, 0, name, stat_info, 0);

  while (entry_size > remaining_size) {
    listing->DoubleCapacity();
    remaining_size = listing->capacity() - listing->size();
  }

  char *buffer;
  bool large_alloc;
  listing->ShareBuffer(&buffer, &large_alloc);
  fuse_add_direntry(req, buffer + listing->size(),
                    remaining_size, name, stat_info,
                    listing->size() + entry_size);
  listing->SetSize(listing->size() + entry_size);
}


/**
 * Open a directory for listing.
 */
static void cvmfs_opendir(fuse_req_t req, fuse_ino_t ino,
                          struct fuse_file_info *fi)
{
  RemountCheck();

  remount_fence_->Enter();
  ino = catalog_manager_->MangleInode(ino);
  LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_opendir on inode: %"PRIu64, ino);

  PathString path;
  catalog::DirectoryEntry d;
  const bool found = GetPathForInode(ino, &path) &&  GetDirentForInode(ino, &d);

  if (!found) {
    remount_fence_->Leave();
    fuse_reply_err(req, ENOENT);
    return;
  }
  if (!d.IsDirectory()) {
    remount_fence_->Leave();
    fuse_reply_err(req, ENOTDIR);
    return;
  }
  LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_opendir on inode: %"PRIu64", path %s",
           ino, path.c_str());

  // Build listing
  BigVector<char> fuse_listing(512);

  // Add current directory link
  struct stat info;
  info = d.GetStatStructure();
  AddToDirListing(req, ".", &info, &fuse_listing);

  // Add parent directory link
  catalog::DirectoryEntry p;
  if (d.inode() != catalog_manager_->GetRootInode() &&
      GetDirentForPath(GetParentPath(path), &p))
  {
    info = p.GetStatStructure();
    AddToDirListing(req, "..", &info, &fuse_listing);
  }

  // Add all names
  catalog::StatEntryList listing_from_catalog;
  bool retval = catalog_manager_->ListingStat(path, &listing_from_catalog);

  if (!retval) {
    remount_fence_->Leave();
    fuse_listing.Clear();  // Buffer is shared, empty manually
    fuse_reply_err(req, EIO);
    return;
  }
  for (unsigned i = 0; i < listing_from_catalog.size(); ++i) {
    // Fix inodes
    PathString entry_path;
    entry_path.Assign(path);
    entry_path.Append("/", 1);
    entry_path.Append(listing_from_catalog.AtPtr(i)->name.GetChars(),
                      listing_from_catalog.AtPtr(i)->name.GetLength());

    catalog::DirectoryEntry entry_dirent;
    if (!GetDirentForPath(entry_path, &entry_dirent)) {
      LogCvmfs(kLogCvmfs, kLogDebug, "listing entry %s vanished, skipping",
               entry_path.c_str());
      continue;
    }

    struct stat fixed_info = listing_from_catalog.AtPtr(i)->info;
    fixed_info.st_ino = entry_dirent.inode();
    AddToDirListing(req, listing_from_catalog.AtPtr(i)->name.c_str(),
                    &fixed_info, &fuse_listing);
  }
  remount_fence_->Leave();

  DirectoryListing stream_listing;
  stream_listing.size = fuse_listing.size();
  stream_listing.capacity = fuse_listing.capacity();
  bool large_alloc;
  fuse_listing.ShareBuffer(&stream_listing.buffer, &large_alloc);
  if (large_alloc)
    stream_listing.capacity = 0;

  // Save the directory listing and return a handle to the listing
  pthread_mutex_lock(&lock_directory_handles_);
  LogCvmfs(kLogCvmfs, kLogDebug,
           "linking directory handle %d to dir inode: %"PRIu64,
           next_directory_handle_, ino);
  (*directory_handles_)[next_directory_handle_] = stream_listing;
  fi->fh = next_directory_handle_;
  ++next_directory_handle_;
  pthread_mutex_unlock(&lock_directory_handles_);
  atomic_inc64(&num_fs_dir_open_);
  atomic_inc32(&open_dirs_);

  fuse_reply_open(req, fi);
}


/**
 * Release a directory.
 */
static void cvmfs_releasedir(fuse_req_t req, fuse_ino_t ino,
                             struct fuse_file_info *fi)
{
  ino = catalog_manager_->MangleInode(ino);
  LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_releasedir on inode %"PRIu64
           ", handle %d", ino, fi->fh);

  int reply = 0;

  pthread_mutex_lock(&lock_directory_handles_);
  DirectoryHandles::iterator iter_handle =
    directory_handles_->find(fi->fh);
  if (iter_handle != directory_handles_->end()) {
    if (iter_handle->second.capacity == 0)
      smunmap(iter_handle->second.buffer);
    else
      free(iter_handle->second.buffer);
    directory_handles_->erase(iter_handle);
    pthread_mutex_unlock(&lock_directory_handles_);
    atomic_dec32(&open_dirs_);
  } else {
    pthread_mutex_unlock(&lock_directory_handles_);
    reply = EINVAL;
  }

  fuse_reply_err(req, reply);
}


/**
 * Very large directory listings have to be sent in slices.
 */
static void ReplyBufferSlice(const fuse_req_t req, const char *buffer,
                             const size_t buffer_size, const off_t offset,
                             const size_t max_size)
{
  if (offset < static_cast<int>(buffer_size)) {
    fuse_reply_buf(req, buffer + offset,
      std::min(static_cast<size_t>(buffer_size - offset), max_size));
  } else {
    fuse_reply_buf(req, NULL, 0);
  }
}


/**
 * Read the directory listing.
 */
static void cvmfs_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                          off_t off, struct fuse_file_info *fi)
{
  LogCvmfs(kLogCvmfs, kLogDebug,
           "cvmfs_readdir on inode %"PRIu64" reading %d bytes from offset %d",
           catalog_manager_->MangleInode(ino), size, off);

  DirectoryListing listing;

  pthread_mutex_lock(&lock_directory_handles_);
  DirectoryHandles::const_iterator iter_handle =
    directory_handles_->find(fi->fh);
  if (iter_handle != directory_handles_->end()) {
    listing = iter_handle->second;
    pthread_mutex_unlock(&lock_directory_handles_);

    ReplyBufferSlice(req, listing.buffer, listing.size, off, size);
    return;
  }

  pthread_mutex_unlock(&lock_directory_handles_);
  fuse_reply_err(req, EINVAL);
}


/**
 * Open a file from cache.  If necessary, file is downloaded first.
 *
 * \return Read-only file descriptor in fi->fh or kChunkedFileHandle for
 * chunked files
 */
static void cvmfs_open(fuse_req_t req, fuse_ino_t ino,
                       struct fuse_file_info *fi)
{
  remount_fence_->Enter();
  ino = catalog_manager_->MangleInode(ino);
  LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_open on inode: %"PRIu64, ino);

  int fd = -1;
  catalog::DirectoryEntry dirent;
  PathString path;

  const bool found = GetDirentForInode(ino, &dirent) &&
                     GetPathForInode(ino, &path);

  if (!found) {
    remount_fence_->Leave();
    fuse_reply_err(req, ENOENT);
    return;
  }
  remount_fence_->Leave();

  // Don't check.  Either done by the OS or one wants to purposefully work
  // around wrong open flags
  //if ((fi->flags & 3) != O_RDONLY) {
  //  fuse_reply_err(req, EROFS);
  //  return;
  //}
#ifdef __APPLE__
  if ((fi->flags & O_SHLOCK) || (fi->flags & O_EXLOCK)) {
    fuse_reply_err(req, EOPNOTSUPP);
    return;
  }
#endif
  if (fi->flags & O_EXCL) {
    fuse_reply_err(req, EEXIST);
    return;
  }

  atomic_inc64(&num_fs_open_);  // Count actual open / fetch operations

  if (dirent.IsChunkedFile()) {
    LogCvmfs(kLogCvmfs, kLogDebug,
             "chunked file %s opened (download delayed to read() call)",
             path.c_str());

    if (atomic_xadd32(&open_files_, 1) >=
        (static_cast<int>(max_open_files_))-kNumReservedFd)
    {
      atomic_dec32(&open_files_);
      LogCvmfs(kLogCvmfs, kLogSyslogErr, "open file descriptor limit exceeded");
      fuse_reply_err(req, EMFILE);
      return;
    }

    chunk_tables_->Lock();
    if (!chunk_tables_->inode2chunks.Contains(ino)) {
      chunk_tables_->Unlock();

      // Retrieve File chunks from the catalog
      FileChunkList *chunks = new FileChunkList();
      if (!dirent.catalog()->ListFileChunks(path, chunks) || chunks->IsEmpty()) {
        LogCvmfs(kLogCvmfs, kLogSyslogErr, "file %s is marked as 'chunked', "
                 "but no chunks found in the catalog %s.", path.c_str(),
                 dirent.catalog()->path().c_str());
        fuse_reply_err(req, EIO);
        return;
      }

      chunk_tables_->Lock();
      // Check again to avoid race
      if (!chunk_tables_->inode2chunks.Contains(ino)) {
        chunk_tables_->inode2chunks.Insert(ino, FileChunkReflist(chunks, path));
        chunk_tables_->inode2references.Insert(ino, 1);
      } else {
        uint32_t refctr;
        bool retval = chunk_tables_->inode2references.Lookup(ino, &refctr);
        assert(retval);
        chunk_tables_->inode2references.Insert(ino, refctr+1);
      }
    } else {
      uint32_t refctr;
      bool retval = chunk_tables_->inode2references.Lookup(ino, &refctr);
      assert(retval);
      chunk_tables_->inode2references.Insert(ino, refctr+1);
    }

    // Update the chunk handle list
    LogCvmfs(kLogCvmfs, kLogDebug,
             "linking chunk handle %d to inode: %"PRIu64,
             chunk_tables_->next_handle, ino);
    chunk_tables_->handle2fd.Insert(chunk_tables_->next_handle, ChunkFd());
    fi->fh = static_cast<uint64_t>(-chunk_tables_->next_handle);
    ++chunk_tables_->next_handle;
    chunk_tables_->Unlock();

    fuse_reply_open(req, fi);
    return;
  }

  fd = cache::FetchDirent(dirent, string(path.GetChars(), path.GetLength()));

  if (fd >= 0) {
    if (atomic_xadd32(&open_files_, 1) <
        (static_cast<int>(max_open_files_))-kNumReservedFd) {
      LogCvmfs(kLogCvmfs, kLogDebug, "file %s opened (fd %d)",
               path.c_str(), fd);
      /*fi->keep_cache = kcache_timeout_ == 0.0 ? 0 : 1;
      if (dirent.cached_mtime() != dirent.mtime()) {
        LogCvmfs(kLogCvmfs, kLogDebug,
                 "file might be new or changed, invalidating cache (%d %d "
                 "%"PRIu64")", dirent.mtime(), dirent.cached_mtime(), ino);
        fi->keep_cache = 0;
        dirent.set_cached_mtime(dirent.mtime());
        inode_cache_->Insert(ino, dirent);
      }*/
      fi->keep_cache = 0;
      fi->fh = fd;
      fuse_reply_open(req, fi);
      return;
    } else {
      if (close(fd) == 0) atomic_dec32(&open_files_);
      LogCvmfs(kLogCvmfs, kLogSyslogErr, "open file descriptor limit exceeded");
      fuse_reply_err(req, EMFILE);
      return;
    }
    assert(false);
  }

  // fd < 0
  LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogErr,
           "failed to open inode: %"PRIu64", CAS key %s, error code %d",
           ino, dirent.checksum().ToString().c_str(), errno);
  if (errno == EMFILE) {
    fuse_reply_err(req, EMFILE);
    return;
  }

  // Prevent Squid DoS
  time_t now = time(NULL);
  if (now - previous_io_error_.timestamp < kForgetDos) {
    SafeSleepMs(previous_io_error_.delay);
    if (previous_io_error_.delay < kMaxIoDelay)
      previous_io_error_.delay *= 2;
  } else {
    // Initial delay
    previous_io_error_.delay = (random() % (kMaxInitIoDelay-1)) + 2;
  }
  previous_io_error_.timestamp = now;

  atomic_inc32(&num_io_error_);
  fuse_reply_err(req, -fd);
}


/**
 * Redirected to pread into cache.
 */
static void cvmfs_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off,
                       struct fuse_file_info *fi)
{
  LogCvmfs(kLogCvmfs, kLogDebug,
           "cvmfs_read on inode: %"PRIu64" reading %d bytes from offset %d fd %d",
           catalog_manager_->MangleInode(ino), size, off, fi->fh);
  atomic_inc64(&num_fs_read_);

  // Get data chunk (<=128k guaranteed by Fuse)
  char *data = static_cast<char *>(alloca(size));
  unsigned int overall_bytes_fetched = 0;

  // Do we have a a chunked file?
  if (static_cast<int64_t>(fi->fh) < 0) {
    const uint64_t chunk_handle =
      static_cast<uint64_t>(-static_cast<int64_t>(fi->fh));
    ChunkFd chunk_fd;
    FileChunkReflist chunks;
    bool retval;

    // Fetch chunk list and file descriptor
    chunk_tables_->Lock();
    retval = chunk_tables_->inode2chunks.Lookup(ino, &chunks);
    assert(retval);
    chunk_tables_->Unlock();

    // Find the chunk that holds the beginning of the requested data
    assert(chunks.list->size() > 0);
    unsigned idx_low = 0, idx_high = chunks.list->size()-1;
    unsigned chunk_idx = idx_high/2;
    while (idx_low < idx_high) {
      if (chunks.list->AtPtr(chunk_idx)->offset() > off) {
        assert(idx_high > 0);
        idx_high = chunk_idx-1;
      } else {
        if ((chunk_idx == chunks.list->size()-1) ||
            chunks.list->AtPtr(chunk_idx+1)->offset() > off)
        {
          break;
        }
        idx_low = chunk_idx + 1;
      }
      chunk_idx = idx_low + (idx_high-idx_low)/2;
    }

    // Lock chunk handle
    pthread_mutex_t *handle_lock = chunk_tables_->Handle2Lock(chunk_handle);
    LockMutex(handle_lock);
    chunk_tables_->Lock();
    retval = chunk_tables_->handle2fd.Lookup(chunk_handle, &chunk_fd);
    assert(retval);
    chunk_tables_->Unlock();

    // Fetch all needed chunks and read the requested data
    off_t offset_in_chunk = off - chunks.list->AtPtr(chunk_idx)->offset();
    do {
      // Open file descriptor to chunk
      if ((chunk_fd.fd == -1) || (chunk_fd.chunk_idx != chunk_idx)) {
        // TODO: read-ahead
        if (chunk_fd.fd != -1) close(chunk_fd.fd);
        string verbose_path = "Part of " + chunks.path.ToString();
        chunk_fd.fd = cache::FetchChunk(*chunks.list->AtPtr(chunk_idx),
                                        verbose_path);
        if (chunk_fd.fd < 0) {
          chunk_fd.fd = -1;
          chunk_tables_->Lock();
          chunk_tables_->handle2fd.Insert(chunk_handle, chunk_fd);
          chunk_tables_->Unlock();
          UnlockMutex(handle_lock);
          fuse_reply_err(req, EIO);
          return;
        }
        chunk_fd.chunk_idx = chunk_idx;
      }

      LogCvmfs(kLogCvmfs, kLogDebug, "reading from chunk fd %d",
               chunk_fd.fd);
      // Read data from chunk
      const size_t bytes_to_read = size - overall_bytes_fetched;
      const size_t remaining_bytes_in_chunk =
        chunks.list->AtPtr(chunk_idx)->size() - offset_in_chunk;
      size_t bytes_to_read_in_chunk =
        std::min(bytes_to_read, remaining_bytes_in_chunk);
      const size_t bytes_fetched =
        pread(chunk_fd.fd, data + overall_bytes_fetched,
              bytes_to_read_in_chunk, offset_in_chunk);

      if (bytes_fetched == (size_t)-1) {
        LogCvmfs(kLogCvmfs, kLogSyslogErr, "read err no %d result %d (%s)",
                 errno, bytes_fetched, chunks.path.ToString().c_str());
        chunk_tables_->Lock();
        chunk_tables_->handle2fd.Insert(chunk_handle, chunk_fd);
        chunk_tables_->Unlock();
        UnlockMutex(handle_lock);
        fuse_reply_err(req, errno);
        return;
      }
      overall_bytes_fetched += bytes_fetched;

      // Proceed to the next chunk to keep on reading data
      ++chunk_idx;
      offset_in_chunk = 0;
    } while ((overall_bytes_fetched < size) &&
             (chunk_idx < chunks.list->size()));

    // Update chunk file descriptor
    chunk_tables_->Lock();
    chunk_tables_->handle2fd.Insert(chunk_handle, chunk_fd);
    chunk_tables_->Unlock();
    UnlockMutex(handle_lock);
    LogCvmfs(kLogCvmfs, kLogDebug, "released chunk file descriptor %d",
             chunk_fd.fd);
  } else {
    const int64_t fd = fi->fh;
    overall_bytes_fetched = pread(fd, data, size, off);
  }

  // Push it to user
  fuse_reply_buf(req, data, overall_bytes_fetched);
  LogCvmfs(kLogCvmfs, kLogDebug, "pushed %d bytes to user",
           overall_bytes_fetched);
}


/**
 * File close operation, redirected into cache.
 */
static void cvmfs_release(fuse_req_t req, fuse_ino_t ino,
                          struct fuse_file_info *fi)
{
  ino = catalog_manager_->MangleInode(ino);
  LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_release on inode: %"PRIu64, ino);
  const int64_t fd = fi->fh;

  // do we have a chunked file?
  if (static_cast<int64_t>(fi->fh) < 0) {
    const uint64_t chunk_handle =
      static_cast<uint64_t>(-static_cast<int64_t>(fi->fh));
    LogCvmfs(kLogCvmfs, kLogDebug, "releasing chunk handle %"PRIu64,
             chunk_handle);
    ChunkFd chunk_fd;
    FileChunkReflist chunks;
    uint32_t refctr;
    bool retval;

    chunk_tables_->Lock();
    retval = chunk_tables_->handle2fd.Lookup(chunk_handle, &chunk_fd);
    assert(retval);
    chunk_tables_->handle2fd.Erase(chunk_handle);

    retval = chunk_tables_->inode2references.Lookup(ino, &refctr);
    assert(retval);
    refctr--;
    if (refctr == 0) {
      LogCvmfs(kLogCvmfs, kLogDebug, "releasing chunk list for inode %"PRIu64,
               ino);
      FileChunkReflist to_delete;
      retval = chunk_tables_->inode2chunks.Lookup(ino, &to_delete);
      assert(retval);
      chunk_tables_->inode2references.Erase(ino);
      chunk_tables_->inode2chunks.Erase(ino);
      delete to_delete.list;
    } else {
      chunk_tables_->inode2references.Insert(ino, refctr);
    }
    chunk_tables_->Unlock();

    if (chunk_fd.fd != -1)
      close(chunk_fd.fd);
    atomic_dec32(&open_files_);
  } else {
    if (close(fd) == 0) {
      atomic_dec32(&open_files_);
    }
  }
  fuse_reply_err(req, 0);
}


static void cvmfs_statfs(fuse_req_t req, fuse_ino_t ino) {
  ino = catalog_manager_->MangleInode(ino);
  LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_statfs on inode: %"PRIu64, ino);

  // If we return 0 it will cause the fs to be ignored in "df"
  struct statvfs info;
  memset(&info, 0, sizeof(info));

  // Unmanaged cache
  if (quota::GetCapacity() == 0) {
    fuse_reply_statfs(req, &info);
    return;
  }

  uint64_t available = 0;
  uint64_t size = quota::GetSize();
  info.f_bsize = 1;

  if (quota::GetCapacity() == (uint64_t)(-1)) {
    // Unrestricted cache, look at free space on cache dir fs
    struct statfs cache_buf;
    if (statfs(".", &cache_buf) == 0) {
      available = cache_buf.f_bavail * cache_buf.f_bsize;
      info.f_blocks = size + available;
    } else {
      info.f_blocks = size;
    }
  } else {
    // Take values from LRU module
    info.f_blocks = quota::GetCapacity();
    available = quota::GetCapacity() - size;
  }

  info.f_bfree = info.f_bavail = available;

  // Inodes / entries
  remount_fence_->Enter();
  info.f_files = catalog_manager_->all_inodes();
  info.f_ffree = info.f_favail =
    catalog_manager_->all_inodes() - catalog_manager_->loaded_inodes();
  remount_fence_->Leave();

  fuse_reply_statfs(req, &info);
}


#ifdef __APPLE__
static void cvmfs_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
                           size_t size, uint32_t position)
#else
static void cvmfs_getxattr(fuse_req_t req, fuse_ino_t ino, const char *name,
                           size_t size)
#endif
{
  remount_fence_->Enter();
  ino = catalog_manager_->MangleInode(ino);
  LogCvmfs(kLogCvmfs, kLogDebug,
           "cvmfs_getxattr on inode: %"PRIu64" for xattr: %s", ino, name);

  const string attr = name;
  catalog::DirectoryEntry d;
  const bool found = GetDirentForInode(ino, &d);
  remount_fence_->Leave();

  if (!found) {
    fuse_reply_err(req, ENOENT);
    return;
  }

  string attribute_value;

  if (attr == "user.pid") {
    attribute_value = StringifyInt(pid_);
  } else if (attr == "user.version") {
    attribute_value = string(VERSION) + "." + string(CVMFS_PATCH_LEVEL);
  } else if (attr == "user.hash") {
    if (!d.checksum().IsNull()) {
      attribute_value = d.checksum().ToString() + " (SHA-1)";
    } else {
      fuse_reply_err(req, ENOATTR);
      return;
    }
  } else if (attr == "user.lhash") {
    if (!d.checksum().IsNull()) {
      string result;
      int fd = cache::Open(d.checksum());
      if (fd < 0) {
        attribute_value = "Not in cache";
      } else {
        hash::Any hash(hash::kSha1);
        FILE *f = fdopen(fd, "r");
        if (!f) {
          fuse_reply_err(req, EIO);
          return;
        }
        if (!zlib::CompressFile2Null(f, &hash)) {
          fclose(f);
          fuse_reply_err(req, EIO);
          return;
        }
        fclose(f);
        attribute_value = hash.ToString() + " (SHA-1)";
      }
    } else {
      fuse_reply_err(req, ENOATTR);
      return;
    }
  } else if (attr == "user.revision") {
    const uint64_t revision = catalog_manager_->GetRevision();
    attribute_value = StringifyInt(revision);
  } else if (attr == "user.root_hash") {
    attribute_value = catalog_manager_->GetRootHash().ToString();
  } else if (attr == "user.expires") {
    if (catalogs_valid_until_ == kIndefiniteDeadline) {
      attribute_value = "never (fixed root catalog)";
    } else {
      time_t now = time(NULL);
      attribute_value = StringifyInt((catalogs_valid_until_-now)/60);
    }
  } else if (attr == "user.maxfd") {
    attribute_value = StringifyInt(max_open_files_ - kNumReservedFd);
  } else if (attr == "user.usedfd") {
    attribute_value = StringifyInt(atomic_read32(&open_files_));
  } else if (attr == "user.useddirp") {
    attribute_value = StringifyInt(atomic_read32(&open_dirs_));
  } else if (attr == "user.nioerr") {
    attribute_value = StringifyInt(atomic_read32(&num_io_error_));
  } else if (attr == "user.proxy") {
    vector< vector<string> > proxy_chain;
    unsigned current_group;
    download::GetProxyInfo(&proxy_chain, &current_group);
    if (proxy_chain.size()) {
      attribute_value = proxy_chain[current_group][0];
    } else {
      attribute_value = "DIRECT";
    }
  } else if (attr == "user.host") {
    vector<string> host_chain;
    vector<int> rtt;
    unsigned current_host;
    download::GetHostInfo(&host_chain, &rtt, &current_host);
    if (host_chain.size()) {
      attribute_value = string(host_chain[current_host]);
    } else {
      attribute_value = "internal error: no hosts defined";
    }
  } else if (attr == "user.uptime") {
    time_t now = time(NULL);
    uint64_t uptime = now - boot_time_;
    attribute_value = StringifyInt(uptime / 60);
  } else if (attr == "user.nclg") {
    const int num_catalogs = catalog_manager_->GetNumCatalogs();
    attribute_value = StringifyInt(num_catalogs);
  } else if (attr == "user.nopen") {
    attribute_value = StringifyInt(atomic_read64(&num_fs_open_));
  } else if (attr == "user.ndiropen") {
    attribute_value = StringifyInt(atomic_read64(&num_fs_dir_open_));
  } else if (attr == "user.ndownload") {
    attribute_value = StringifyInt(cache::GetNumDownloads());
  } else if (attr == "user.timeout") {
    unsigned seconds, seconds_direct;
    download::GetTimeout(&seconds, &seconds_direct);
    attribute_value = StringifyInt(seconds);
  } else if (attr == "user.timeout_direct") {
    unsigned seconds, seconds_direct;
    download::GetTimeout(&seconds, &seconds_direct);
    attribute_value = StringifyInt(seconds_direct);
  } else if (attr == "user.rx") {
    int64_t rx = uint64_t(download::GetStatistics().transferred_bytes);
    attribute_value = StringifyInt(rx/1024);
  } else if (attr == "user.speed") {
    int64_t rx = uint64_t(download::GetStatistics().transferred_bytes);
    int64_t time = uint64_t(download::GetStatistics().transfer_time);
    if (time == 0)
      attribute_value = "n/a";
    else
      attribute_value = StringifyInt((rx/1024)/time);
  } else if (attr == "user.fqrn") {
    attribute_value = *repository_name_;
  } else {
    fuse_reply_err(req, ENOATTR);
    return;
  }

  if (size == 0) {
    fuse_reply_xattr(req, attribute_value.length());
  } else if (size >= attribute_value.length()) {
    fuse_reply_buf(req, &attribute_value[0], attribute_value.length());
  } else {
    fuse_reply_err(req, ERANGE);
  }
}


static void cvmfs_listxattr(fuse_req_t req, fuse_ino_t ino, size_t size) {
  remount_fence_->Enter();
  ino = catalog_manager_->MangleInode(ino);
  LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_listxattr on inode: %"PRIu64", size %u",
           ino, size);

  catalog::DirectoryEntry d;
  const bool found = GetDirentForInode(ino, &d);
  remount_fence_->Leave();

  if (!found) {
    fuse_reply_err(req, ENOENT);
    return;
  }

  const char base_list[] = "user.pid\0user.version\0user.revision\0"
    "user.root_hash\0user.expires\0user.maxfd\0user.usedfd\0user.nioerr\0"
    "user.host\0user.proxy\0user.uptime\0user.nclg\0user.nopen\0user.ndownload\0"
    "user.timeout\0user.timeout_direct\0user.rx\0user.speed\0user.fqrn\0"
    "user.ndiropen\0";
  string attribute_list(base_list, sizeof(base_list)-1);
  if (!d.checksum().IsNull()) {
    const char regular_file_list[] = "user.hash\0user.lhash\0";
    attribute_list += string(regular_file_list, sizeof(regular_file_list)-1);
  }

  if (size == 0) {
    fuse_reply_xattr(req, attribute_list.length());
  } else if (size >= attribute_list.length()) {
    fuse_reply_buf(req, &attribute_list[0], attribute_list.length());
  } else {
    fuse_reply_err(req, ERANGE);
  }
}


bool Evict(const string &path) {
  catalog::DirectoryEntry dirent;
  remount_fence_->Enter();
  const bool found = GetDirentForPath(PathString(path), &dirent);
  remount_fence_->Leave();

  if (!found || !dirent.IsRegular())
    return false;
  quota::Remove(dirent.checksum());
  return true;
}


bool Pin(const string &path) {
  catalog::DirectoryEntry dirent;
  remount_fence_->Enter();
  const bool found = GetDirentForPath(PathString(path), &dirent);
  remount_fence_->Leave();

  if (!found || !dirent.IsRegular())
    return false;
  if (dirent.IsChunkedFile()) {
    FileChunkList chunks;
    dirent.catalog()->ListFileChunks(PathString(path), &chunks);
    for (unsigned i = 0; i < chunks.size(); ++i) {
      bool retval =
        quota::Pin(chunks.AtPtr(i)->content_hash(), chunks.AtPtr(i)->size(),
                   "Part of " + path, false);
      if (!retval)
        return false;
      int fd = cache::FetchChunk(*chunks.AtPtr(i), "Part of " + path);
      if (fd < 0) {
        quota::Unpin(chunks.AtPtr(i)->content_hash());
        return false;
      }
      retval =
        quota::Pin(chunks.AtPtr(i)->content_hash(), chunks.AtPtr(i)->size(),
                   "Part of " + path, false);
      close(fd);
      if (!retval)
        return false;
    }
    return true;
  }

  bool retval = quota::Pin(dirent.checksum(), dirent.size(), path, false);
  if (!retval)
    return false;
  int fd = cache::FetchDirent(dirent, path);
  if (fd < 0) {
    quota::Unpin(dirent.checksum());
    return false;
  }
  // Again because it was overwritten by FetchDirent
  retval = quota::Pin(dirent.checksum(), dirent.size(), path, false);
  close(fd);
  return retval;
}


/**
 * Do after-daemon() initialization
 */
static void cvmfs_init(void *userdata, struct fuse_conn_info *conn) {
  LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_init");

  // NFS support
#ifdef CVMFS_NFS_SUPPORT
  conn->want |= FUSE_CAP_EXPORT_SUPPORT;
#endif
}

static void cvmfs_destroy(void *unused __attribute__((unused))) {
  // The debug log is already closed at this point
  LogCvmfs(kLogCvmfs, kLogDebug, "cvmfs_destroy");
}

/**
 * Puts the callback functions in one single structure
 */
static void SetCvmfsOperations(struct fuse_lowlevel_ops *cvmfs_operations) {
  memset(cvmfs_operations, 0, sizeof(*cvmfs_operations));

  // Init/Fini
  cvmfs_operations->init     = cvmfs_init;
  cvmfs_operations->destroy  = cvmfs_destroy;

  cvmfs_operations->lookup      = cvmfs_lookup;
  cvmfs_operations->getattr     = cvmfs_getattr;
  cvmfs_operations->readlink    = cvmfs_readlink;
  cvmfs_operations->open        = cvmfs_open;
  cvmfs_operations->read        = cvmfs_read;
  cvmfs_operations->release     = cvmfs_release;
  cvmfs_operations->opendir     = cvmfs_opendir;
  cvmfs_operations->readdir     = cvmfs_readdir;
  cvmfs_operations->releasedir  = cvmfs_releasedir;
  cvmfs_operations->statfs      = cvmfs_statfs;
  cvmfs_operations->getxattr    = cvmfs_getxattr;
  cvmfs_operations->listxattr   = cvmfs_listxattr;
  cvmfs_operations->forget      = cvmfs_forget;
}

}  // namespace cvmfs


bool g_options_ready = false;
bool g_download_ready = false;
bool g_cache_ready = false;
bool g_nfs_maps_ready = false;
bool g_peers_ready = false;
bool g_monitor_ready = false;
bool g_signature_ready = false;
bool g_quota_ready = false;
bool g_talk_ready = false;
bool g_running_created = false;

int g_fd_lockfile = -1;
void *g_sqlite_scratch = NULL;
void *g_sqlite_page_cache = NULL;
string *g_boot_error = NULL;

__attribute__ ((visibility ("default")))
loader::CvmfsExports *g_cvmfs_exports = NULL;


static int Init(const loader::LoaderExports *loader_exports) {
  int retval;
  g_boot_error = new string("unknown error");
  cvmfs::loader_exports_ = loader_exports;

  uint64_t mem_cache_size = cvmfs::kDefaultMemcache;
  unsigned timeout = cvmfs::kDefaultTimeout;
  unsigned timeout_direct = cvmfs::kDefaultTimeout;
  unsigned proxy_reset_after = 0;
  unsigned host_reset_after = 0;
  unsigned max_retries = 1;
  unsigned backoff_init = 2000;
  unsigned backoff_max = 10000;
  string tracefile = "";
  string cachedir = string(cvmfs::kDefaultCachedir);
  unsigned max_ttl = 0;
  int kcache_timeout = 0;
  bool diskless = false;
  bool rebuild_cachedb = false;
  bool nfs_source = false;
  bool nfs_shared = false;
  string nfs_shared_dir = string(cvmfs::kDefaultCachedir);
  bool shared_cache = false;
  int64_t quota_limit = cvmfs::kDefaultCacheSizeMb;
  string hostname = "localhost";
  string proxies = "";
  string dns_server = "";
  string public_keys = "";
  bool ignore_signature = false;
  string root_hash = "";
  string repository_tag = "";
  map<uint64_t, uint64_t> uid_map;
  map<uint64_t, uint64_t> gid_map;

  cvmfs::boot_time_ = loader_exports->boot_time;

  // Option parsing
  options::Init();
  if (loader_exports->config_files != "") {
    vector<string> tokens = SplitString(loader_exports->config_files, ':');
    for (unsigned i = 0, s = tokens.size(); i < s; ++i) {
      options::ParsePath(tokens[i]);
    }
  } else {
    options::ParseDefault(loader_exports->repository_name);
  }
  g_options_ready = true;
  string parameter;

  // Logging
  if (options::GetValue("CVMFS_SYSLOG_LEVEL", &parameter))
    SetLogSyslogLevel(String2Uint64(parameter));
  else
    SetLogSyslogLevel(3);
  if (options::GetValue("CVMFS_SYSLOG_FACILITY", &parameter))
    SetLogSyslogFacility(String2Int64(parameter));
  if (options::GetValue("CVMFS_USYSLOG", &parameter))
    SetLogMicroSyslog(parameter);
  if (options::GetValue("CVMFS_DEBUGLOG", &parameter))
    SetLogDebugFile(parameter);
  SetLogSyslogPrefix(loader_exports->repository_name);

  LogCvmfs(kLogCvmfs, kLogDebug, "Options:\n%s", options::Dump().c_str());

  // Overwrite default options
  if (options::GetValue("CVMFS_MEMCACHE_SIZE", &parameter))
    mem_cache_size = String2Uint64(parameter) * 1024*1024;
  if (options::GetValue("CVMFS_TIMEOUT", &parameter))
    timeout = String2Uint64(parameter);
  if (options::GetValue("CVMFS_TIMEOUT_DIRECT", &parameter))
    timeout_direct = String2Uint64(parameter);
  if (options::GetValue("CVMFS_PROXY_RESET_AFTER", &parameter))
    proxy_reset_after = String2Uint64(parameter);
  if (options::GetValue("CVMFS_HOST_RESET_AFTER", &parameter))
    host_reset_after = String2Uint64(parameter);
  if (options::GetValue("CVMFS_MAX_RETRIES", &parameter))
    max_retries = String2Uint64(parameter);
  if (options::GetValue("CVMFS_BACKOFF_INIT", &parameter))
    backoff_init = String2Uint64(parameter)*1000;
  if (options::GetValue("CVMFS_BACKOFF_MAX", &parameter))
    backoff_max = String2Uint64(parameter)*1000;
  if (options::GetValue("CVMFS_TRACEFILE", &parameter))
    tracefile = parameter;
  if (options::GetValue("CVMFS_MAX_TTL", &parameter))
    max_ttl = String2Uint64(parameter);
  if (options::GetValue("CVMFS_KCACHE_TIMEOUT", &parameter))
    kcache_timeout = String2Int64(parameter);
  if (options::GetValue("CVMFS_QUOTA_LIMIT", &parameter))
    quota_limit = String2Int64(parameter) * 1024*1024;
  if (options::GetValue("CVMFS_HTTP_PROXY", &parameter))
    proxies = parameter;
  if (options::GetValue("CVMFS_DNS_SERVER", &parameter))
    dns_server = parameter;
  if (options::GetValue("CVMFS_KEYS_DIR", &parameter)) {
    // Collect .pub files from CVMFS_KEYS_DIR
    public_keys = JoinStrings(FindFiles(parameter, ".pub"), ":");
  } else if (options::GetValue("CVMFS_PUBLIC_KEY", &parameter)) {
    public_keys = parameter;
  } else {
    public_keys = JoinStrings(FindFiles("/etc/cvmfs/keys", ".pub"), ":");
  }
  if (options::GetValue("CVMFS_ROOT_HASH", &parameter))
    root_hash = parameter;
  if (options::GetValue("CVMFS_REPOSITORY_TAG", &parameter))
    repository_tag = parameter;
  if (options::GetValue("CVMFS_DISKLESS", &parameter) &&
      options::IsOn(parameter))
  {
    diskless = true;
  }
  if (options::GetValue("CVMFS_NFS_SOURCE", &parameter) &&
      options::IsOn(parameter))
  {
    nfs_source = true;
    if (options::GetValue("CVMFS_NFS_SHARED", &parameter))
    {
      nfs_shared = true;
      nfs_shared_dir = MakeCanonicalPath(parameter);
    }
  }
  if (options::GetValue("CVMFS_IGNORE_SIGNATURE", &parameter) &&
      options::IsOn(parameter))
  {
    ignore_signature = true;
  }
  if (options::GetValue("CVMFS_AUTO_UPDATE", &parameter) &&
      !options::IsOn(parameter))
  {
    cvmfs::fixed_catalog_ = true;
  }
  if (options::GetValue("CVMFS_SERVER_URL", &parameter)) {
    vector<string> tokens = SplitString(loader_exports->repository_name, '.');
    const string org = tokens[0];
    hostname = parameter;
    hostname = ReplaceAll(hostname, "@org@", org);
    hostname = ReplaceAll(hostname, "@fqrn@", loader_exports->repository_name);
  }
  if (options::GetValue("CVMFS_CACHE_BASE", &parameter)) {
    cachedir = MakeCanonicalPath(parameter);
    if (options::GetValue("CVMFS_SHARED_CACHE", &parameter) &&
        options::IsOn(parameter))
    {
      shared_cache = true;
      cachedir = cachedir + "/shared";
    } else {
      shared_cache = false;
      cachedir = cachedir + "/" + loader_exports->repository_name;
    }
  }
  if (options::GetValue("CVMFS_UID_MAP", &parameter)) {
    retval = options::ParseUIntMap(parameter, &uid_map);
    if (!retval) {
      *g_boot_error = "failed to parse uid map " + parameter;
      return loader::kFailOptions;
    }
  }
  if (options::GetValue("CVMFS_GID_MAP", &parameter)) {
    retval = options::ParseUIntMap(parameter, &gid_map);
    if (!retval) {
      *g_boot_error = "failed to parse gid map " + parameter;
      return loader::kFailOptions;
    }
  }


  // Fill cvmfs option variables from configuration
  cvmfs::foreground_ = loader_exports->foreground;
  cvmfs::cachedir_ = new string(cachedir);
  cvmfs::nfs_shared_dir_ = new string(nfs_shared_dir);
  cvmfs::tracefile_ = new string(tracefile);
  cvmfs::repository_name_ = new string(loader_exports->repository_name);
  cvmfs::repository_tag_ = new string(repository_tag);
  cvmfs::mountpoint_ = new string(loader_exports->mount_point);
  g_uid = geteuid();
  g_gid = getegid();
  cvmfs::max_ttl_ = max_ttl;
  if (kcache_timeout) {
    cvmfs::kcache_timeout_ =
      (kcache_timeout == -1) ? 0.0 : double(kcache_timeout);
  }
  LogCvmfs(kLogCvmfs, kLogDebug, "kernel caches expire after %d seconds",
           int(cvmfs::kcache_timeout_));

  // Tune SQlite3
  sqlite3_shutdown();  // Make sure SQlite starts clean after initialization
  retval = sqlite3_config(SQLITE_CONFIG_MULTITHREAD);
  assert(retval == SQLITE_OK);
  g_sqlite_scratch = smalloc(8192*16);  // 8 KB for 8 threads (2 slots per thread)
  g_sqlite_page_cache = smalloc(1280*3275);  // 4MB
  retval = sqlite3_config(SQLITE_CONFIG_SCRATCH, g_sqlite_scratch, 8192, 16);
  assert(retval == SQLITE_OK);
  retval = sqlite3_config(SQLITE_CONFIG_PAGECACHE, g_sqlite_page_cache,
                          1280, 3275);
  assert(retval == SQLITE_OK);
  // 4 KB
  retval = sqlite3_config(SQLITE_CONFIG_LOOKASIDE, 32, 128);
  assert(retval == SQLITE_OK);

  // Meta-data memory caches
  const double memcache_unit_size =
    7.0 * lru::Md5PathCache::GetEntrySize() +
    lru::InodeCache::GetEntrySize() + lru::PathCache::GetEntrySize();
  const unsigned memcache_num_units =
    mem_cache_size / static_cast<unsigned>(memcache_unit_size);
  // Number of cache entries must be a multiple of 64
  const unsigned mask_64 = ~((1 << 6) - 1);
  cvmfs::inode_cache_ = new lru::InodeCache(memcache_num_units & mask_64);
  cvmfs::path_cache_ = new lru::PathCache(memcache_num_units & mask_64);
  cvmfs::md5path_cache_ =
    new lru::Md5PathCache((memcache_num_units*7) & mask_64);
  cvmfs::inode_tracker_ = new glue::InodeTracker();

  cvmfs::directory_handles_ = new cvmfs::DirectoryHandles();
  cvmfs::directory_handles_->set_empty_key((uint64_t)(-1));
  cvmfs::directory_handles_->set_deleted_key((uint64_t)(-2));
  cvmfs::chunk_tables_ = new ChunkTables();

  // Runtime counters
  atomic_init64(&cvmfs::num_fs_open_);
  atomic_init64(&cvmfs::num_fs_dir_open_);
  atomic_init64(&cvmfs::num_fs_lookup_);
  atomic_init64(&cvmfs::num_fs_lookup_negative_);
  atomic_init64(&cvmfs::num_fs_stat_);
  atomic_init64(&cvmfs::num_fs_read_);
  atomic_init64(&cvmfs::num_fs_readlink_);
  atomic_init64(&cvmfs::num_fs_forget_);
  atomic_init32(&cvmfs::num_io_error_);
  cvmfs::previous_io_error_.timestamp = 0;
  cvmfs::previous_io_error_.delay = 0;

  // Create cache directory, if necessary
  if (!MkdirDeep(*cvmfs::cachedir_, 0700)) {
    *g_boot_error = "cannot create cache directory " + *cvmfs::cachedir_;
    return loader::kFailCacheDir;
  }

  // Spawn / connect to peer server
  if (diskless) {
    if (!peers::Init(GetParentPath(*cvmfs::cachedir_),
                     loader_exports->program_name, ""))
    {
      *g_boot_error = "failed to initialize peer socket";
      return loader::kFailPeers;
    }
  }
  g_peers_ready = true;

  // Try to jump to cache directory.  This tests, if it is accassible.
  // Also, it brings speed later on.
  if (chdir(cvmfs::cachedir_->c_str()) != 0) {
    *g_boot_error = "cache directory " + *cvmfs::cachedir_ + " is unavailable";
    return loader::kFailCacheDir;
  }

  // Create lock file and running sentinel
  g_fd_lockfile = TryLockFile("lock." + *cvmfs::repository_name_);
  if (g_fd_lockfile == -1) {
    *g_boot_error = "could not acquire lock (" + StringifyInt(errno) + ")";
    return loader::kFailCacheDir;
  } else if (g_fd_lockfile == -2) {
    // Prevent double mount
    string fqrn;
    retval = platform_getxattr(*cvmfs::mountpoint_, "user.fqrn", &fqrn);
    if (!retval) {
      g_fd_lockfile = LockFile("lock." + *cvmfs::repository_name_);
      if (g_fd_lockfile < 0) {
        *g_boot_error = "could not acquire lock (" + StringifyInt(errno) + ")";
        return loader::kFailCacheDir;
      }
    } else {
      if (fqrn == *cvmfs::repository_name_) {
        LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogWarn,
                 "repository already mounted on %s",
                 cvmfs::mountpoint_->c_str());
        return loader::kFailDoubleMount;
      } else {
        LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogErr,
                 "CernVM-FS repository %s already mounted on %s",
                 fqrn.c_str(), cvmfs::mountpoint_->c_str());
        return loader::kFailOtherMount;

      }
    }
  }
  platform_stat64 info;
  if (platform_stat(("running." + *cvmfs::repository_name_).c_str(),
                    &info) == 0)
  {
    LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslogWarn, "looks like cvmfs has been "
             "crashed previously, rebuilding cache database");
    rebuild_cachedb = true;
  }
  retval = open(("running." + *cvmfs::repository_name_).c_str(),
                O_RDONLY | O_CREAT, 0600);
  if (retval < 0) {
    *g_boot_error = "could not open running sentinel (" +
                    StringifyInt(errno) + ")";
    return loader::kFailCacheDir;
  }
  close(retval);
  g_running_created = true;

  // Creates a set of cache directories (256 directories named 00..ff)
  if (!cache::Init(".")) {
    *g_boot_error = "Failed to setup cache in " + *cvmfs::cachedir_ +
                    ": " + strerror(errno);
    return loader::kFailCacheDir;
  }
  CreateFile("./.cvmfscache", 0600);
  g_cache_ready = true;

  // Start NFS maps module, if necessary
#ifdef CVMFS_NFS_SUPPORT
  if (nfs_source) {
    if (FileExists("./no_nfs_maps." + (*cvmfs::repository_name_))) {
      *g_boot_error = "Cache was used without NFS maps before. "
                      "It has to be wiped out.";
      return loader::kFailNfsMaps;
    }

    cvmfs::nfs_maps_ = true;

    string inode_cache_dir = "./nfs_maps." + (*cvmfs::repository_name_);
    if (nfs_shared) {
      inode_cache_dir = (*cvmfs::nfs_shared_dir_) + "/nfs_maps."
                          + (*cvmfs::repository_name_);
    }
    if (!MkdirDeep(inode_cache_dir, 0700)) {
      *g_boot_error = "Failed to initialize NFS maps";
      return loader::kFailNfsMaps;
    }
    if (!nfs_maps::Init(inode_cache_dir,
                        catalog::AbstractCatalogManager::kInodeOffset+1,
                        rebuild_cachedb, nfs_shared))
    {
      *g_boot_error = "Failed to initialize NFS maps";
      return loader::kFailNfsMaps;
    }
    g_nfs_maps_ready = true;
  } else {
    CreateFile("./no_nfs_maps." + (*cvmfs::repository_name_), 0600);
  }
#endif

  // Init quota / managed cache
  if (quota_limit < 0)
    quota_limit = 0;
  int64_t quota_threshold = quota_limit/2;
  if (shared_cache) {
    if (!quota::InitShared(loader_exports->program_name, ".",
                           (uint64_t)quota_limit, (uint64_t)quota_threshold))
    {
      *g_boot_error = "Failed to initialize shared lru cache";
      return loader::kFailQuota;
    }
  } else {
    if (!quota::Init(".", (uint64_t)quota_limit, (uint64_t)quota_threshold,
                     rebuild_cachedb))
    {
      *g_boot_error = "Failed to initialize lru cache";
      return loader::kFailQuota;
    }
  }
  g_quota_ready = true;

  if (quota::GetSize() > quota::GetCapacity()) {
    LogCvmfs(kLogCvmfs, kLogDebug | kLogSyslog,
             "cache is already beyond quota size "
             "(size: %"PRId64", capacity: %"PRId64"), cleaning up",
             quota::GetSize(), quota::GetCapacity());
    if (!quota::Cleanup(quota_threshold)) {
      *g_boot_error = "Failed to clean up cache";
      return loader::kFailQuota;
    }
  }
  if (quota_limit) {
    LogCvmfs(kLogCvmfs, kLogDebug,
             "CernVM-FS: quota initialized, current size %luMB",
             quota::GetSize()/(1024*1024));
  }

  // Monitor, check for maximum number of open files
  if (cvmfs::UseWatchdog()) {
    if (!monitor::Init(".", *cvmfs::repository_name_, true)) {
      *g_boot_error = "failed to initialize watchdog.";
      return loader::kFailMonitor;
    }
    g_monitor_ready = true;
  }
  cvmfs::max_open_files_ = monitor::GetMaxOpenFiles();
  atomic_init32(&cvmfs::open_files_);
  atomic_init32(&cvmfs::open_dirs_);

  // Control & command interface
  if (!talk::Init(".")) {
    *g_boot_error = "failed to initialize talk socket (" +
                    StringifyInt(errno) + ")";
    return loader::kFailTalk;
  }
  g_talk_ready = true;

  // Network initialization
  download::Init(cvmfs::kDefaultNumConnections, false);
  download::SetHostChain(hostname);
  download::SetProxyChain(proxies);
  if (! dns_server.empty()) {
    download::SetDnsServer(dns_server);
  }
  download::SetTimeout(timeout, timeout_direct);
  download::SetProxyGroupResetDelay(proxy_reset_after);
  download::SetHostResetDelay(host_reset_after);
  download::SetRetryParameters(max_retries, backoff_init, backoff_max);
  g_download_ready = true;

  signature::Init();
  if (!signature::LoadPublicRsaKeys(public_keys)) {
    *g_boot_error = "failed to load public key(s)";
    return loader::kFailSignature;
  } else {
    LogCvmfs(kLogCvmfs, kLogDebug, "CernVM-FS: using public key(s) %s",
             public_keys.c_str());
  }
  g_signature_ready = true;
  if (FileExists("/etc/cvmfs/blacklist")) {
    if (!signature::LoadBlacklist("/etc/cvmfs/blacklist")) {
      *g_boot_error = "failed to load blacklist";
      return loader::kFailSignature;
    }
  }

  // Load initial file catalog
  LogCvmfs(kLogCvmfs, kLogDebug, "fuse inode size is %d bits",
           sizeof(fuse_ino_t) * 8);
  cvmfs::inode_annotation_ = new catalog::InodeGenerationAnnotation();
  cvmfs::catalog_manager_ =
      new cache::CatalogManager(*cvmfs::repository_name_, ignore_signature);
  if (!nfs_source) {
    cvmfs::catalog_manager_->SetInodeAnnotation(cvmfs::inode_annotation_);
  }
  cvmfs::catalog_manager_->SetOwnerMaps(uid_map, gid_map);

  // Load specific tag (root hash has precedence)
  if ((root_hash == "") && (*cvmfs::repository_tag_ != "")) {
    manifest::ManifestEnsemble ensemble;
    retval = manifest::Fetch("", *cvmfs::repository_name_, 0, NULL, &ensemble);
    if (retval != manifest::kFailOk) {
      *g_boot_error = "Failed to fetch manifest";
      return loader::kFailHistory;
    }
    hash::Any history_hash = ensemble.manifest->history();
    if (history_hash.IsNull()) {
      *g_boot_error = "No history";
      return loader::kFailHistory;
    }
    string history_path = "txn/historydb" + history_hash.ToString() + "." +
                          *cvmfs::repository_name_;
    string history_url = "/data" + history_hash.MakePath(1, 2) + "H";
    download::JobInfo download_history(&history_url, true, true, &history_path,
                                       &history_hash);
    retval = download::Fetch(&download_history);
    if (retval != download::kFailOk) {
      *g_boot_error = "failed to download history: " + StringifyInt(retval);
      return loader::kFailHistory;
    }
    history::Database tag_db;
    history::TagList tag_list;
    retval = tag_db.Open(history_path, sqlite::kDbOpenReadOnly) &&
             tag_list.Load(&tag_db);
    unlink(history_path.c_str());
    if (!retval) {
      *g_boot_error = "failed to open history";
      return loader::kFailHistory;
    }
    history::Tag tag;
    retval = tag_list.FindTag(*cvmfs::repository_tag_, &tag);
    if (!retval) {
      *g_boot_error = "no such tag: " + *cvmfs::repository_tag_;
      return loader::kFailHistory;
    }
    root_hash = tag.root_hash.ToString();
  }

  if (root_hash != "") {
    cvmfs::fixed_catalog_ = true;
    hash::Any hash(hash::kSha1, hash::HexPtr(string(root_hash)));
    retval = cvmfs::catalog_manager_->InitFixed(hash);
  } else {
    retval = cvmfs::catalog_manager_->Init();
  }
  if (!retval) {
    *g_boot_error = "Failed to initialize root file catalog";
    return loader::kFailCatalog;
  }
  cvmfs::inode_generation_info_.initial_revision =
    cvmfs::catalog_manager_->GetRevision();
  LogCvmfs(kLogCvmfs, kLogDebug, "root inode is %"PRIu64,
           cvmfs::catalog_manager_->GetRootInode());

  cvmfs::remount_fence_ = new cvmfs::RemountFence();

  return loader::kFailOk;
}


/**
 * Things that have to be executed after fork() / daemon()
 */
static void Spawn() {
  int retval;

  // Setup catalog reload alarm (_after_ fork())
  atomic_init32(&cvmfs::maintenance_mode_);
  atomic_init32(&cvmfs::drainout_mode_);
  atomic_init32(&cvmfs::reload_critical_section_);
  atomic_init32(&cvmfs::catalogs_expired_);
  if (!cvmfs::fixed_catalog_) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = cvmfs::AlarmReload;
    sa.sa_flags = SA_SIGINFO;
    sigfillset(&sa.sa_mask);
    retval = sigaction(SIGALRM, &sa, NULL);
    assert(retval == 0);
    unsigned ttl = cvmfs::catalog_manager_->offline_mode() ?
      cvmfs::kShortTermTTL : cvmfs::GetEffectiveTTL();
    alarm(ttl);
    cvmfs::catalogs_valid_until_ = time(NULL) + ttl;
  } else {
    cvmfs::catalogs_valid_until_ = cvmfs::kIndefiniteDeadline;
  }

  cvmfs::pid_ = getpid();
  if (cvmfs::UseWatchdog() && g_monitor_ready) {
    monitor::Spawn();
  }
  download::Spawn();
  quota::Spawn();
  talk::Spawn();
  if (cvmfs::nfs_maps_)
    nfs_maps::Spawn();

  if (*cvmfs::tracefile_ != "")
    tracer::Init(8192, 7000, *cvmfs::tracefile_);
  else
    tracer::InitNull();
}


static string GetErrorMsg() {
  if (g_boot_error)
    return *g_boot_error;
  return "";
}


static void Fini() {
  signal(SIGALRM, SIG_DFL);
  tracer::Fini();
  if (g_signature_ready) signature::Fini();
  if (g_download_ready) download::Fini();
  if (g_talk_ready) talk::Fini();
  if (g_monitor_ready) monitor::Fini();
  if (g_quota_ready) quota::Fini();
  if (g_nfs_maps_ready) nfs_maps::Fini();
  if (g_cache_ready) cache::Fini();
  if (g_running_created)
    unlink(("running." + *cvmfs::repository_name_).c_str());
  if (g_fd_lockfile >= 0) UnlockFile(g_fd_lockfile);
  if (g_peers_ready) peers::Fini();
  if (g_options_ready) options::Fini();

  delete cvmfs::remount_fence_;
  delete cvmfs::catalog_manager_;
  delete cvmfs::inode_annotation_;
  delete cvmfs::directory_handles_;
  delete cvmfs::chunk_tables_;
  delete cvmfs::inode_tracker_;
  delete cvmfs::path_cache_;
  delete cvmfs::inode_cache_;
  delete cvmfs::md5path_cache_;
  delete cvmfs::cachedir_;
  delete cvmfs::nfs_shared_dir_;
  delete cvmfs::tracefile_;
  delete cvmfs::repository_name_;
  delete cvmfs::repository_tag_;
  delete cvmfs::mountpoint_;
  cvmfs::remount_fence_ = NULL;
  cvmfs::catalog_manager_ = NULL;
  cvmfs::inode_annotation_ = NULL;
  cvmfs::directory_handles_ = NULL;
  cvmfs::chunk_tables_ = NULL;
  cvmfs::inode_tracker_ = NULL;
  cvmfs::path_cache_ = NULL;
  cvmfs::inode_cache_ = NULL;
  cvmfs::md5path_cache_ = NULL;
  cvmfs::cachedir_ = NULL;
  cvmfs::nfs_shared_dir_ = NULL;
  cvmfs::tracefile_ = NULL;
  cvmfs::repository_name_ = NULL;
  cvmfs::repository_tag_ = NULL;
  cvmfs::mountpoint_= NULL;

  sqlite3_shutdown();
  if (g_sqlite_page_cache) free(g_sqlite_page_cache);
  if (g_sqlite_scratch) free(g_sqlite_scratch);
  g_sqlite_page_cache = NULL;
  g_sqlite_scratch = NULL;

  delete g_boot_error;
  g_boot_error = NULL;
  SetLogSyslogPrefix("");
  SetLogMicroSyslog("");
  SetLogDebugFile("");
}


static int AltProcessFlavor(int argc, char **argv) {
  if (strcmp(argv[1], "__peersrv__") == 0) {
    return peers::MainPeerServer(argc, argv);
  }
  if (strcmp(argv[1], "__cachemgr__") == 0) {
    return quota::MainCacheManager(argc, argv);
  }
  return 1;
}


static bool MaintenanceMode(const int fd_progress) {
  SendMsg2Socket(fd_progress, "Entering maintenance mode\n");
  signal(SIGALRM, SIG_DFL);
  atomic_cas32(&cvmfs::maintenance_mode_, 0, 1);
  string msg_progress = "Draining out kernel caches (" +
                        StringifyInt((int)cvmfs::kcache_timeout_) + "s)\n";
  SendMsg2Socket(fd_progress, msg_progress);
  SafeSleepMs((int)cvmfs::kcache_timeout_*1000 + cvmfs::kReloadSafetyMargin);
  return true;
}


static bool SaveState(const int fd_progress, loader::StateList *saved_states) {
  string msg_progress;

  unsigned num_open_dirs = cvmfs::directory_handles_->size();
  if (num_open_dirs != 0) {
#ifdef DEBUGMSG
    for (cvmfs::DirectoryHandles::iterator i = cvmfs::directory_handles_->begin(),
         iEnd = cvmfs::directory_handles_->end(); i != iEnd; ++i)
    {
      LogCvmfs(kLogCvmfs, kLogDebug, "saving dirhandle %d", i->first);
    }
#endif

    msg_progress = "Saving open directory handles (" +
      StringifyInt(num_open_dirs) + " handles)\n";
    SendMsg2Socket(fd_progress, msg_progress);

    // TODO: should rather be saved just in a malloc'd memory block
    cvmfs::DirectoryHandles *saved_handles =
      new cvmfs::DirectoryHandles(*cvmfs::directory_handles_);
    loader::SavedState *save_open_dirs = new loader::SavedState();
    save_open_dirs->state_id = loader::kStateOpenDirs;
    save_open_dirs->state = saved_handles;
    saved_states->push_back(save_open_dirs);
  }

  if (!cvmfs::nfs_maps_) {
    msg_progress = "Saving inode tracker\n";
    SendMsg2Socket(fd_progress, msg_progress);
    glue::InodeTracker *saved_inode_tracker =
      new glue::InodeTracker(*cvmfs::inode_tracker_);
    loader::SavedState *state_glue_buffer = new loader::SavedState();
    state_glue_buffer->state_id = loader::kStateGlueBufferV3;
    state_glue_buffer->state = saved_inode_tracker;
    saved_states->push_back(state_glue_buffer);
  }

  msg_progress = "Saving chunk tables\n";
  SendMsg2Socket(fd_progress, msg_progress);
  ChunkTables *saved_chunk_tables = new ChunkTables(*cvmfs::chunk_tables_);
  loader::SavedState *state_chunk_tables = new loader::SavedState();
  state_chunk_tables->state_id = loader::kStateOpenFiles;
  state_chunk_tables->state = saved_chunk_tables;
  saved_states->push_back(state_chunk_tables);

  msg_progress = "Saving inode generation\n";
  SendMsg2Socket(fd_progress, msg_progress);
  cvmfs::inode_generation_info_.inode_generation +=
    cvmfs::catalog_manager_->inode_gauge();
  cvmfs::InodeGenerationInfo *saved_inode_generation =
    new cvmfs::InodeGenerationInfo(cvmfs::inode_generation_info_);
  loader::SavedState *state_inode_generation = new loader::SavedState();
  state_inode_generation->state_id = loader::kStateInodeGeneration;
  state_inode_generation->state = saved_inode_generation;
  saved_states->push_back(state_inode_generation);

  msg_progress = "Saving open files counter\n";
  SendMsg2Socket(fd_progress, msg_progress);
  uint32_t *saved_num_fd = new uint32_t(cvmfs::open_files_);
  loader::SavedState *state_num_fd = new loader::SavedState();
  state_num_fd->state_id = loader::kStateOpenFilesCounter;
  state_num_fd->state = saved_num_fd;
  saved_states->push_back(state_num_fd);

  return true;
}


static bool RestoreState(const int fd_progress,
                         const loader::StateList &saved_states)
{
  for (unsigned i = 0, l = saved_states.size(); i < l; ++i) {
    if (saved_states[i]->state_id == loader::kStateOpenDirs) {
      SendMsg2Socket(fd_progress, "Restoring open directory handles... ");
      delete cvmfs::directory_handles_;
      cvmfs::DirectoryHandles *saved_handles =
        (cvmfs::DirectoryHandles *)saved_states[i]->state;
      cvmfs::directory_handles_ = new cvmfs::DirectoryHandles(*saved_handles);
      cvmfs::open_dirs_ = cvmfs::directory_handles_->size();
      cvmfs::DirectoryHandles::const_iterator i =
        cvmfs::directory_handles_->begin();
      for (; i != cvmfs::directory_handles_->end(); ++i) {
        if (i->first >= cvmfs::next_directory_handle_)
          cvmfs::next_directory_handle_ = i->first + 1;
      }

      SendMsg2Socket(fd_progress,
        StringifyInt(cvmfs::directory_handles_->size()) + " handles\n");
    }

    if (saved_states[i]->state_id == loader::kStateGlueBuffer) {
      SendMsg2Socket(fd_progress, "Migrating inode tracker (v1 to v3)... ");
      compat::inode_tracker::InodeTracker *saved_inode_tracker =
        (compat::inode_tracker::InodeTracker *)saved_states[i]->state;
      compat::inode_tracker::Migrate(saved_inode_tracker, cvmfs::inode_tracker_);
      SendMsg2Socket(fd_progress, " done\n");
    }

    if (saved_states[i]->state_id == loader::kStateGlueBufferV2) {
      SendMsg2Socket(fd_progress, "Migrating inode tracker (v2 to v3)... ");
      compat::inode_tracker_v2::InodeTracker *saved_inode_tracker =
        (compat::inode_tracker_v2::InodeTracker *)saved_states[i]->state;
      compat::inode_tracker_v2::Migrate(saved_inode_tracker,
                                        cvmfs::inode_tracker_);
      SendMsg2Socket(fd_progress, " done\n");
    }

    if (saved_states[i]->state_id == loader::kStateGlueBufferV3) {
      SendMsg2Socket(fd_progress, "Restoring inode tracker... ");
      delete cvmfs::inode_tracker_;
      glue::InodeTracker *saved_inode_tracker =
        (glue::InodeTracker *)saved_states[i]->state;
      cvmfs::inode_tracker_ = new glue::InodeTracker(*saved_inode_tracker);
      SendMsg2Socket(fd_progress, " done\n");
    }

    if (saved_states[i]->state_id == loader::kStateOpenFiles) {
      SendMsg2Socket(fd_progress, "Restoring chunk tables... ");
      delete cvmfs::chunk_tables_;
      ChunkTables *saved_chunk_tables = (ChunkTables *)saved_states[i]->state;
      cvmfs::chunk_tables_ = new ChunkTables(*saved_chunk_tables);
      SendMsg2Socket(fd_progress, " done\n");
    }

    if (saved_states[i]->state_id == loader::kStateInodeGeneration) {
      SendMsg2Socket(fd_progress, "Restoring inode generation... ");
      cvmfs::InodeGenerationInfo *old_info =
        (cvmfs::InodeGenerationInfo *)saved_states[i]->state;
      if (old_info->version == 1) {
        // Migration
        cvmfs::inode_generation_info_.initial_revision =
          old_info->initial_revision;
        cvmfs::inode_generation_info_.incarnation = old_info->incarnation;
        // Note: in the rare case of inode generation being 0 before, inode
        // can clash after reload before remount
      } else {
        cvmfs::inode_generation_info_ = *old_info;
      }
      ++cvmfs::inode_generation_info_.incarnation;
      SendMsg2Socket(fd_progress, " done\n");
    }

    if (saved_states[i]->state_id == loader::kStateOpenFilesCounter) {
      SendMsg2Socket(fd_progress, "Restoring open files counter... ");
      cvmfs::open_files_ = *((uint32_t *)saved_states[i]->state);
      SendMsg2Socket(fd_progress, " done\n");
    }
  }
  if (cvmfs::inode_annotation_) {
    uint64_t saved_generation = cvmfs::inode_generation_info_.inode_generation;
    cvmfs::inode_annotation_->IncGeneration(saved_generation);
  }

  return true;
}


static void FreeSavedState(const int fd_progress,
                           const loader::StateList &saved_states)
{
  for (unsigned i = 0, l = saved_states.size(); i < l; ++i) {
    switch (saved_states[i]->state_id) {
      case loader::kStateOpenDirs:
        SendMsg2Socket(fd_progress, "Releasing saved open directory handles\n");
        delete static_cast<cvmfs::DirectoryHandles *>(saved_states[i]->state);
        break;
      case loader::kStateGlueBuffer:
        SendMsg2Socket(fd_progress, "Releasing saved glue buffer (version 1)\n");
        delete static_cast<compat::inode_tracker::InodeTracker *>(
          saved_states[i]->state);
        break;
      case loader::kStateGlueBufferV2:
        SendMsg2Socket(fd_progress, "Releasing saved glue buffer (version 2)\n");
        delete static_cast<compat::inode_tracker_v2::InodeTracker *>(
          saved_states[i]->state);
        break;
      case loader::kStateGlueBufferV3:
        SendMsg2Socket(fd_progress, "Releasing saved glue buffer\n");
        delete static_cast<glue::InodeTracker *>(saved_states[i]->state);
        break;
      case loader::kStateOpenFiles:
        SendMsg2Socket(fd_progress, "Releasing chunk tables\n");
        delete static_cast<ChunkTables *>(saved_states[i]->state);
        break;
      case loader::kStateInodeGeneration:
        SendMsg2Socket(fd_progress, "Releasing saved inode generation info\n");
        delete static_cast<cvmfs::InodeGenerationInfo *>(saved_states[i]->state);
        break;
      case loader::kStateOpenFilesCounter:
        SendMsg2Socket(fd_progress, "Releasing open files counter\n");
        delete static_cast<uint32_t *>(saved_states[i]->state);
        break;
      default:
        break;
    }
  }
}


static void __attribute__((constructor)) LibraryMain() {
  g_cvmfs_exports = new loader::CvmfsExports();
  g_cvmfs_exports->so_version = PACKAGE_VERSION;
  g_cvmfs_exports->fnAltProcessFlavor = AltProcessFlavor;
  g_cvmfs_exports->fnInit = Init;
  g_cvmfs_exports->fnSpawn = Spawn;
  g_cvmfs_exports->fnFini = Fini;
  g_cvmfs_exports->fnGetErrorMsg = GetErrorMsg;
  g_cvmfs_exports->fnMaintenanceMode = MaintenanceMode;
  g_cvmfs_exports->fnSaveState = SaveState;
  g_cvmfs_exports->fnRestoreState = RestoreState;
  g_cvmfs_exports->fnFreeSavedState = FreeSavedState;
  cvmfs::SetCvmfsOperations(&g_cvmfs_exports->cvmfs_operations);
}


static void __attribute__((destructor)) LibraryExit() {
  delete g_cvmfs_exports;
  g_cvmfs_exports = NULL;
}
