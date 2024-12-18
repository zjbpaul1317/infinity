// Copyright(C) 2023 InfiniFlow, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

module;

#include <filesystem>
#include <functional>
#include <regex>
#include <string>

module storage;

import config;
import stl;
import buffer_manager;
import default_values;
import wal_manager;
import catalog;
import txn_manager;
import builtin_functions;
import third_party;
import logger;

import txn;
import infinity_exception;
import status;
import background_process;
import compaction_process;
import object_storage_process;
import status;
import bg_task;
import periodic_trigger_thread;
import periodic_trigger;
import log_file;

import query_context;
import infinity_context;
import memindex_tracer;
import cleanup_scanner;
import persistence_manager;
import extra_ddl_info;
import virtual_store;
import result_cache_manager;
import global_resource_usage;

namespace infinity {

Storage::Storage(Config *config_ptr) : config_ptr_(config_ptr) {
#ifdef INFINITY_DEBUG
    GlobalResourceUsage::IncrObjectCount("Storage");
#endif
}

Storage::~Storage() {
#ifdef INFINITY_DEBUG
    GlobalResourceUsage::DecrObjectCount("Storage");
#endif
}

ResultCacheManager *Storage::result_cache_manager() const noexcept {
    if (config_ptr_->ResultCache() != "on") {
        return nullptr;
    }
    return result_cache_manager_.get();
}

ResultCacheManager *Storage::GetResultCacheManagerPtr() const noexcept { return result_cache_manager_.get(); }

StorageMode Storage::GetStorageMode() const {
    std::unique_lock<std::mutex> lock(mutex_);
    return current_storage_mode_;
}

Status Storage::SetStorageMode(StorageMode target_mode) {
    StorageMode current_mode = GetStorageMode();
    if (current_mode == target_mode) {
        LOG_WARN(fmt::format("Set unchanged mode"));
        return Status::OK();
    }
    cleanup_info_tracer_ = MakeUnique<CleanupInfoTracer>();
    switch (current_mode) {
        case StorageMode::kUnInitialized: {
            if (target_mode != StorageMode::kAdmin) {
                UnrecoverableError("Attempt to set storage mode from UnInit to UnInit");
            }

            {
                std::unique_lock<std::mutex> lock(mutex_);
                current_storage_mode_ = target_mode;
            }

            // Construct wal manager
            if (wal_mgr_ != nullptr) {
                UnrecoverableError("WAL manager was initialized before.");
            }

            wal_mgr_ = MakeUnique<WalManager>(this,
                                              config_ptr_->WALDir(),
                                              config_ptr_->DataDir(),
                                              config_ptr_->WALCompactThreshold(),
                                              config_ptr_->DeltaCheckpointThreshold(),
                                              config_ptr_->FlushMethodAtCommit());
            LOG_INFO(fmt::format("Set storage from un-init mode to admin"));
            break;
        }
        case StorageMode::kAdmin: {
            if (target_mode == StorageMode::kAdmin) {
                UnrecoverableError("Attempt to set storage mode from Admin to Admin");
            }

            if (target_mode == StorageMode::kUnInitialized) {
                wal_mgr_.reset();
                LOG_INFO(fmt::format("Set storage from admin mode to un-init"));
                break;
            }

            {
                std::unique_lock<std::mutex> lock(mutex_);
                current_storage_mode_ = target_mode;
            }

            switch (config_ptr_->StorageType()) {
                case StorageType::kLocal: {
                    // Not init remote store
                    break;
                }
                case StorageType::kMinio: {
                    if (VirtualStore::IsInit()) {
                        UnrecoverableError("remote storage system was initialized before.");
                    }
                    LOG_INFO(fmt::format("Init remote store url: {}", config_ptr_->ObjectStorageUrl()));
                    Status status = VirtualStore::InitRemoteStore(StorageType::kMinio,
                                                                  config_ptr_->ObjectStorageUrl(),
                                                                  config_ptr_->ObjectStorageHttps(),
                                                                  config_ptr_->ObjectStorageAccessKey(),
                                                                  config_ptr_->ObjectStorageSecretKey(),
                                                                  config_ptr_->ObjectStorageBucket());
                    if (!status.ok()) {
                        {
                            std::unique_lock<std::mutex> lock(mutex_);
                            current_storage_mode_ = current_mode;
                        }
                        VirtualStore::UnInitRemoteStore();
                        return status;
                    }

                    if (object_storage_processor_ != nullptr) {
                        UnrecoverableError("Object storage processor was initialized before.");
                    }
                    object_storage_processor_ = MakeUnique<ObjectStorageProcess>();
                    object_storage_processor_->Start();
                    break;
                }
                default: {
                    UnrecoverableError(fmt::format("Unsupported storage type: {}.", ToString(config_ptr_->StorageType())));
                }
            }
            // Construct persistence store
            String persistence_dir = config_ptr_->PersistenceDir();
            if (!persistence_dir.empty()) {
                if (persistence_manager_ != nullptr) {
                    UnrecoverableError("persistence_manager was initialized before.");
                }
                i64 persistence_object_size_limit = config_ptr_->PersistenceObjectSizeLimit();
                persistence_manager_ = MakeUnique<PersistenceManager>(persistence_dir, config_ptr_->DataDir(), (SizeT)persistence_object_size_limit);
            }

            SizeT cache_result_num = config_ptr_->CacheResultNum();
            if (result_cache_manager_ == nullptr) {
                result_cache_manager_ = MakeUnique<ResultCacheManager>(cache_result_num);
            }

            // Construct buffer manager
            if (buffer_mgr_ != nullptr) {
                UnrecoverableError("Buffer manager was initialized before.");
            }
            buffer_mgr_ = MakeUnique<BufferManager>(config_ptr_->BufferManagerSize(),
                                                    MakeShared<String>(config_ptr_->DataDir()),
                                                    MakeShared<String>(config_ptr_->TempDir()),
                                                    persistence_manager_.get(),
                                                    config_ptr_->LRUNum());
            buffer_mgr_->Start();

            if (current_storage_mode_ == StorageMode::kReadable) {
                LOG_INFO("No checkpoint found in READER mode, waiting for log replication");
                reader_init_phase_ = ReaderInitPhase::kPhase1;
                return Status::OK();
            }

            // Must init catalog before txn manager.
            // Replay wal file wrap init catalog
            TxnTimeStamp system_start_ts = wal_mgr_->ReplayWalFile(target_mode);
            if (system_start_ts == 0) {
                // Init database, need to create default_db
                LOG_INFO(fmt::format("Init a new catalog"));
                new_catalog_ = Catalog::NewCatalog();
            }

            i64 compact_interval = config_ptr_->CompactInterval() > 0 ? config_ptr_->CompactInterval() : 0;
            if (compact_interval > 0 and current_storage_mode_ == StorageMode::kWritable) {
                LOG_INFO(fmt::format("Init compaction alg"));
                new_catalog_->InitCompactionAlg(system_start_ts);
            } else {
                LOG_INFO(fmt::format("Skip init compaction alg"));
            }

            BuiltinFunctions builtin_functions(new_catalog_);
            builtin_functions.Init();
            // Catalog finish init here.
            if (bg_processor_ != nullptr) {
                UnrecoverableError("Background processor was initialized before.");
            }
            bg_processor_ = MakeUnique<BGTaskProcessor>(wal_mgr_.get(), new_catalog_.get());

            // Construct txn manager
            if (txn_mgr_ != nullptr) {
                UnrecoverableError("Transaction manager was initialized before.");
            }
            txn_mgr_ = MakeUnique<TxnManager>(buffer_mgr_.get(), wal_mgr_.get(), system_start_ts);
            txn_mgr_->Start();

            // start WalManager after TxnManager since it depends on TxnManager.
            wal_mgr_->Start();

            if (system_start_ts == 0 && target_mode == StorageMode::kWritable) {
                CreateDefaultDB();
            }

            if (memory_index_tracer_ != nullptr) {
                UnrecoverableError("Memory index tracer was initialized before.");
            }
            memory_index_tracer_ = MakeUnique<BGMemIndexTracer>(config_ptr_->MemIndexMemoryQuota(), new_catalog_.get(), txn_mgr_.get());

            bg_processor_->Start();

            if (target_mode == StorageMode::kWritable) {
                // Compact processor will do in WRITABLE MODE:
                // 1. Compact segments into a big one
                // 2. Scan which segments should be merged into one
                // 3. Save the dumped mem index in catalog

                if (compact_processor_ != nullptr) {
                    UnrecoverableError("compact processor was initialized before.");
                }

                compact_processor_ = MakeUnique<CompactionProcessor>(new_catalog_.get(), txn_mgr_.get());
                compact_processor_->Start();
            }

            // recover index after start compact process
            new_catalog_->StartMemoryIndexCommit();
            new_catalog_->MemIndexRecover(buffer_mgr_.get(), system_start_ts);

            if (periodic_trigger_thread_ != nullptr) {
                UnrecoverableError("periodic trigger was initialized before.");
            }
            periodic_trigger_thread_ = MakeUnique<PeriodicTriggerThread>();

            i64 optimize_interval = config_ptr_->OptimizeIndexInterval() > 0 ? config_ptr_->OptimizeIndexInterval() : 0;
            i64 cleanup_interval = config_ptr_->CleanupInterval() > 0 ? config_ptr_->CleanupInterval() : 0;
            i64 full_checkpoint_interval_sec = config_ptr_->FullCheckpointInterval() > 0 ? config_ptr_->FullCheckpointInterval() : 0;
            i64 delta_checkpoint_interval_sec = config_ptr_->DeltaCheckpointInterval() > 0 ? config_ptr_->DeltaCheckpointInterval() : 0;

            if (target_mode == StorageMode::kWritable) {
                periodic_trigger_thread_->full_checkpoint_trigger_ =
                    MakeShared<CheckpointPeriodicTrigger>(full_checkpoint_interval_sec, wal_mgr_.get(), true);
                periodic_trigger_thread_->delta_checkpoint_trigger_ =
                    MakeShared<CheckpointPeriodicTrigger>(delta_checkpoint_interval_sec, wal_mgr_.get(), false);
                periodic_trigger_thread_->compact_segment_trigger_ =
                    MakeShared<CompactSegmentPeriodicTrigger>(compact_interval, compact_processor_.get());
                periodic_trigger_thread_->optimize_index_trigger_ =
                    MakeShared<OptimizeIndexPeriodicTrigger>(optimize_interval, compact_processor_.get());
            }

            periodic_trigger_thread_->cleanup_trigger_ =
                MakeShared<CleanupPeriodicTrigger>(cleanup_interval, bg_processor_.get(), new_catalog_.get(), txn_mgr_.get());
            bg_processor_->SetCleanupTrigger(periodic_trigger_thread_->cleanup_trigger_);

            if (target_mode == StorageMode::kWritable) {
                auto txn = txn_mgr_->BeginTxn(MakeUnique<String>("ForceCheckpointTask"));
                auto force_ckp_task = MakeShared<ForceCheckpointTask>(txn, true, system_start_ts);
                bg_processor_->Submit(force_ckp_task);
                force_ckp_task->Wait();
                txn->SetReaderAllowed(true);
                txn_mgr_->CommitTxn(txn);
            } else {
                reader_init_phase_ = ReaderInitPhase::kPhase2;
            }

            periodic_trigger_thread_->Start();
            break;
        }
        case StorageMode::kReadable: {
            if (target_mode == StorageMode::kReadable) {
                UnrecoverableError("Attempt to set storage mode from Readable to Readable");
            }

            if (target_mode == StorageMode::kUnInitialized or target_mode == StorageMode::kAdmin) {

                if (periodic_trigger_thread_ != nullptr) {
                    if (reader_init_phase_ != ReaderInitPhase::kPhase2) {
                        UnrecoverableError("Error reader init phase");
                    }
                    periodic_trigger_thread_->Stop();
                    periodic_trigger_thread_.reset();
                }

                if (compact_processor_ != nullptr) {
                    UnrecoverableError("Compact processor shouldn't be set before");
                }

                if (bg_processor_ != nullptr) {
                    if (reader_init_phase_ != ReaderInitPhase::kPhase2) {
                        UnrecoverableError("Error reader init phase");
                    }
                    bg_processor_->Stop();
                    bg_processor_.reset();
                }

                new_catalog_.reset();

                memory_index_tracer_.reset();

                if (wal_mgr_ != nullptr) {
                    wal_mgr_->Stop();
                    wal_mgr_.reset();
                }

                switch (config_ptr_->StorageType()) {
                    case StorageType::kLocal: {
                        // Not init remote store
                        break;
                    }
                    case StorageType::kMinio: {
                        if (object_storage_processor_ != nullptr) {
                            object_storage_processor_->Stop();
                            object_storage_processor_.reset();
                            VirtualStore::UnInitRemoteStore();
                        }
                        break;
                    }
                    default: {
                        UnrecoverableError(fmt::format("Unsupported storage type: {}.", ToString(config_ptr_->StorageType())));
                    }
                }

                if (txn_mgr_ != nullptr) {
                    if (reader_init_phase_ != ReaderInitPhase::kPhase2) {
                        UnrecoverableError("Error reader init phase");
                    }
                    txn_mgr_->Stop();
                    txn_mgr_.reset();
                }

                if (buffer_mgr_ != nullptr) {
                    buffer_mgr_->Stop();
                    buffer_mgr_.reset();
                }

                persistence_manager_.reset();

                if (target_mode == StorageMode::kAdmin) {
                    // wal_manager stop won't reset many member. We need to recreate the wal_manager object.
                    wal_mgr_ = MakeUnique<WalManager>(this,
                                                      config_ptr_->WALDir(),
                                                      config_ptr_->DataDir(),
                                                      config_ptr_->WALCompactThreshold(),
                                                      config_ptr_->DeltaCheckpointThreshold(),
                                                      config_ptr_->FlushMethodAtCommit());
                }
            }

            if (target_mode == StorageMode::kWritable) {
                if (compact_processor_ != nullptr) {
                    UnrecoverableError("compact processor was initialized before.");
                }

                compact_processor_ = MakeUnique<CompactionProcessor>(new_catalog_.get(), txn_mgr_.get());
                compact_processor_->Start();

                periodic_trigger_thread_->Stop();
                i64 compact_interval = config_ptr_->CompactInterval() > 0 ? config_ptr_->CompactInterval() : 0;
                i64 optimize_interval = config_ptr_->OptimizeIndexInterval() > 0 ? config_ptr_->OptimizeIndexInterval() : 0;
                //                i64 cleanup_interval = config_ptr_->CleanupInterval() > 0 ? config_ptr_->CleanupInterval() : 0;
                i64 full_checkpoint_interval_sec = config_ptr_->FullCheckpointInterval() > 0 ? config_ptr_->FullCheckpointInterval() : 0;
                i64 delta_checkpoint_interval_sec = config_ptr_->DeltaCheckpointInterval() > 0 ? config_ptr_->DeltaCheckpointInterval() : 0;
                periodic_trigger_thread_->full_checkpoint_trigger_ =
                    MakeShared<CheckpointPeriodicTrigger>(full_checkpoint_interval_sec, wal_mgr_.get(), true);
                periodic_trigger_thread_->delta_checkpoint_trigger_ =
                    MakeShared<CheckpointPeriodicTrigger>(delta_checkpoint_interval_sec, wal_mgr_.get(), false);
                periodic_trigger_thread_->compact_segment_trigger_ =
                    MakeShared<CompactSegmentPeriodicTrigger>(compact_interval, compact_processor_.get());
                periodic_trigger_thread_->optimize_index_trigger_ =
                    MakeShared<OptimizeIndexPeriodicTrigger>(optimize_interval, compact_processor_.get());
                periodic_trigger_thread_->Start();
            }

            {
                std::unique_lock<std::mutex> lock(mutex_);
                current_storage_mode_ = target_mode;
            }
            break;
        }
        case StorageMode::kWritable: {
            if (target_mode == StorageMode::kWritable) {
                UnrecoverableError("Attempt to set storage mode from Writable to Writable");
            }

            if (target_mode == StorageMode::kUnInitialized or target_mode == StorageMode::kAdmin) {

                if (periodic_trigger_thread_ != nullptr) {
                    periodic_trigger_thread_->Stop();
                    periodic_trigger_thread_.reset();
                }

                if (compact_processor_ != nullptr) {
                    compact_processor_->Stop(); // Different from Readable
                    compact_processor_.reset(); // Different from Readable
                }

                if (bg_processor_ != nullptr) {
                    bg_processor_->Stop();
                    bg_processor_.reset();
                }

                new_catalog_.reset();

                memory_index_tracer_.reset();

                if (wal_mgr_ != nullptr) {
                    wal_mgr_->Stop();
                    wal_mgr_.reset();
                }

                switch (config_ptr_->StorageType()) {
                    case StorageType::kLocal: {
                        // Not init remote store
                        break;
                    }
                    case StorageType::kMinio: {
                        if (object_storage_processor_ != nullptr) {
                            object_storage_processor_->Stop();
                            object_storage_processor_.reset();
                            VirtualStore::UnInitRemoteStore();
                        }
                        break;
                    }
                    default: {
                        UnrecoverableError(fmt::format("Unsupported storage type: {}.", ToString(config_ptr_->StorageType())));
                    }
                }

                if (txn_mgr_ != nullptr) {
                    txn_mgr_->Stop();
                    txn_mgr_.reset();
                }

                if (buffer_mgr_ != nullptr) {
                    buffer_mgr_->Stop();
                    buffer_mgr_.reset();
                }

                persistence_manager_.reset();

                if (target_mode == StorageMode::kAdmin) {
                    // wal_manager stop won't reset many member. We need to recreate the wal_manager object.
                    wal_mgr_ = MakeUnique<WalManager>(this,
                                                      config_ptr_->WALDir(),
                                                      config_ptr_->DataDir(),
                                                      config_ptr_->WALCompactThreshold(),
                                                      config_ptr_->DeltaCheckpointThreshold(),
                                                      config_ptr_->FlushMethodAtCommit());
                }
            }

            if (target_mode == StorageMode::kReadable) {
                if (periodic_trigger_thread_ != nullptr) {
                    periodic_trigger_thread_->Stop();
                    periodic_trigger_thread_.reset();
                }

                if (compact_processor_ != nullptr) {
                    compact_processor_->Stop(); // Different from Readable
                    compact_processor_.reset(); // Different from Readable
                }

                i64 cleanup_interval = config_ptr_->CleanupInterval() > 0 ? config_ptr_->CleanupInterval() : 0;

                periodic_trigger_thread_ = MakeUnique<PeriodicTriggerThread>();
                periodic_trigger_thread_->cleanup_trigger_ =
                    MakeShared<CleanupPeriodicTrigger>(cleanup_interval, bg_processor_.get(), new_catalog_.get(), txn_mgr_.get());
                bg_processor_->SetCleanupTrigger(periodic_trigger_thread_->cleanup_trigger_);
                periodic_trigger_thread_->Start();
            }

            {
                std::unique_lock<std::mutex> lock(mutex_);
                current_storage_mode_ = target_mode;
            }

            break;
        }
    }
    return Status::OK();
}

Status Storage::SetReaderStorageContinue(TxnTimeStamp system_start_ts) {
    StorageMode current_mode = GetStorageMode();
    if (current_mode != StorageMode::kReadable) {
        UnrecoverableError(fmt::format("Expect current storage mode is READER, but it is {}", ToString(current_mode)));
    }

    BuiltinFunctions builtin_functions(new_catalog_);
    builtin_functions.Init();
    // Catalog finish init here.
    if (bg_processor_ != nullptr) {
        UnrecoverableError("Background processor was initialized before.");
    }
    bg_processor_ = MakeUnique<BGTaskProcessor>(wal_mgr_.get(), new_catalog_.get());

    // Construct txn manager
    if (txn_mgr_ != nullptr) {
        UnrecoverableError("Transaction manager was initialized before.");
    }
    txn_mgr_ = MakeUnique<TxnManager>(buffer_mgr_.get(), wal_mgr_.get(), system_start_ts);
    txn_mgr_->Start();

    // start WalManager after TxnManager since it depends on TxnManager.
    wal_mgr_->Start();

    if (memory_index_tracer_ != nullptr) {
        UnrecoverableError("Memory index tracer was initialized before.");
    }
    memory_index_tracer_ = MakeUnique<BGMemIndexTracer>(config_ptr_->MemIndexMemoryQuota(), new_catalog_.get(), txn_mgr_.get());

    new_catalog_->StartMemoryIndexCommit();
    new_catalog_->MemIndexRecover(buffer_mgr_.get(), system_start_ts);

    bg_processor_->Start();

    if (periodic_trigger_thread_ != nullptr) {
        UnrecoverableError("periodic trigger was initialized before.");
    }
    periodic_trigger_thread_ = MakeUnique<PeriodicTriggerThread>();

    i64 cleanup_interval = config_ptr_->CleanupInterval() > 0 ? config_ptr_->CleanupInterval() : 0;
    periodic_trigger_thread_->cleanup_trigger_ =
        MakeShared<CleanupPeriodicTrigger>(cleanup_interval, bg_processor_.get(), new_catalog_.get(), txn_mgr_.get());
    bg_processor_->SetCleanupTrigger(periodic_trigger_thread_->cleanup_trigger_);

    periodic_trigger_thread_->Start();
    reader_init_phase_ = ReaderInitPhase::kPhase2;

    return Status::OK();
}

void Storage::AttachCatalog(const FullCatalogFileInfo &full_ckp_info, const Vector<DeltaCatalogFileInfo> &delta_ckp_infos) {
    new_catalog_ = Catalog::LoadFromFiles(full_ckp_info, delta_ckp_infos, buffer_mgr_.get());
}

void Storage::LoadFullCheckpoint(const String &checkpoint_path) {
    if (new_catalog_.get() != nullptr) {
        UnrecoverableError("Catalog was already initialized before.");
    }
    new_catalog_ = Catalog::LoadFullCheckpoint(checkpoint_path);
}
void Storage::AttachDeltaCheckpoint(const String &checkpoint_path) { new_catalog_->AttachDeltaCheckpoint(checkpoint_path); }

void Storage::CreateDefaultDB() {
    Txn *new_txn = txn_mgr_->BeginTxn(MakeUnique<String>("create db1"));
    new_txn->SetReaderAllowed(true);
    // Txn1: Create db1, OK
    Status status = new_txn->CreateDatabase(MakeShared<String>("default_db"), ConflictType::kError, MakeShared<String>("Initial startup created"));
    if (!status.ok()) {
        UnrecoverableError("Can't initial 'default_db'");
    }
    txn_mgr_->CommitTxn(new_txn);
}

} // namespace infinity
