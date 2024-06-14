#include "config.h"

#include <Common/ProfileEvents.h>
#include <Common/CurrentMetrics.h>
#include <Common/ZooKeeper/ZooKeeper.h>
#include <Common/logger_useful.h>
#include <Common/getRandomASCIIString.h>
#include <Storages/S3Queue/S3QueueSource.h>
#include <Storages/VirtualColumnUtils.h>
#include <Processors/Executors/PullingPipelineExecutor.h>


namespace CurrentMetrics
{
    extern const Metric StorageS3Threads;
    extern const Metric StorageS3ThreadsActive;
}

namespace ProfileEvents
{
    extern const Event S3QueuePullMicroseconds;
}

namespace DB
{

namespace ErrorCodes
{
    extern const int S3_ERROR;
    extern const int NOT_IMPLEMENTED;
    extern const int LOGICAL_ERROR;
}

StorageS3QueueSource::S3QueueObjectInfo::S3QueueObjectInfo(
        const ObjectInfo & object_info,
        Metadata::FileMetadataPtr file_metadata_)
    : ObjectInfo(object_info.relative_path, object_info.metadata)
    , file_metadata(file_metadata_)
{
}

StorageS3QueueSource::FileIterator::FileIterator(
    std::shared_ptr<S3QueueMetadata> metadata_,
    std::unique_ptr<GlobIterator> glob_iterator_,
    std::atomic<bool> & shutdown_called_,
    LoggerPtr logger_)
    : StorageObjectStorageSource::IIterator("S3QueueIterator")
    , metadata(metadata_)
    , glob_iterator(std::move(glob_iterator_))
    , shutdown_called(shutdown_called_)
    , log(logger_)
{
}

bool StorageS3QueueSource::FileIterator::isFinished() const
{
     return iterator_finished
         && listed_keys_cache.end() == std::find_if(
             listed_keys_cache.begin(), listed_keys_cache.end(),
             [](const auto & v) { return !v.second.keys.empty(); })
         && objects_to_retry.empty();
}

size_t StorageS3QueueSource::FileIterator::estimatedKeysCount()
{
    throw Exception(ErrorCodes::NOT_IMPLEMENTED, "Method estimateKeysCount is not implemented");
}

StorageS3QueueSource::ObjectInfoPtr StorageS3QueueSource::FileIterator::nextImpl(size_t processor)
{
    ObjectInfoPtr object_info;
    S3QueueOrderedFileMetadata::BucketInfoPtr bucket_info;

    while (!shutdown_called)
    {
        if (metadata->useBucketsForProcessing())
        {
            std::tie(object_info, bucket_info) = getNextKeyFromAcquiredBucket(processor);
        }
        else
        {
            if (objects_to_retry.empty())
            {
                object_info = glob_iterator->next(processor);
            }
            else
            {
                object_info = objects_to_retry.front();
                objects_to_retry.pop_front();
            }
        }

        if (!object_info)
            return {};

        if (shutdown_called)
        {
            LOG_TEST(log, "Shutdown was called, stopping file iterator");
            return {};
        }

        auto file_metadata = metadata->getFileMetadata(object_info->relative_path, bucket_info);
        if (file_metadata->setProcessing())
            return std::make_shared<S3QueueObjectInfo>(*object_info, file_metadata);
    }
    return {};
}

void StorageS3QueueSource::FileIterator::returnForRetry(ObjectInfoPtr object_info)
{
    if (metadata->useBucketsForProcessing())
    {
        const auto bucket = metadata->getBucketForPath(object_info->relative_path);
        listed_keys_cache[bucket].keys.emplace_front(object_info);
    }
    else
    {
        objects_to_retry.push_back(object_info);
    }
}


std::pair<StorageS3QueueSource::ObjectInfoPtr, S3QueueOrderedFileMetadata::BucketInfoPtr>
StorageS3QueueSource::FileIterator::getNextKeyFromAcquiredBucket(size_t processor)
{
    /// We need this lock to maintain consistency between listing s3 directory
    /// and getting/putting result into listed_keys_cache.
    std::lock_guard lock(buckets_mutex);

    auto bucket_holder_it = bucket_holders.emplace(processor, nullptr).first;
    auto current_processor = toString(processor);

    LOG_TEST(
        log, "Current processor: {}, acquired bucket: {}",
        processor, bucket_holder_it->second ? toString(bucket_holder_it->second->getBucket()) : "None");

    while (true)
    {
        /// Each processing thread gets next path from glob_iterator->next()
        /// and checks if corresponding bucket is already acquired by someone.
        /// In case it is already acquired, they put the key into listed_keys_cache,
        /// so that the thread who acquired the bucket will be able to see
        /// those keys without the need to list s3 directory once again.
        if (bucket_holder_it->second)
        {
            const auto bucket = bucket_holder_it->second->getBucket();
            auto it = listed_keys_cache.find(bucket);
            if (it != listed_keys_cache.end())
            {
                /// `bucket_keys` -- keys we iterated so far and which were not taken for processing.
                /// `bucket_processor` -- processor id of the thread which has acquired the bucket.
                auto & [bucket_keys, bucket_processor] = it->second;

                /// Check correctness just in case.
                if (!bucket_processor.has_value())
                {
                    bucket_processor = current_processor;
                }
                else if (bucket_processor.value() != current_processor)
                {
                    throw Exception(
                        ErrorCodes::LOGICAL_ERROR,
                        "Expected current processor {} to be equal to {} for bucket {}",
                        current_processor,
                        bucket_processor.has_value() ? toString(bucket_processor.value()) : "None",
                        bucket);
                }

                /// Take next key to process
                if (!bucket_keys.empty())
                {
                    /// Take the key from the front, the order is important.
                    auto object_info = bucket_keys.front();
                    bucket_keys.pop_front();

                    LOG_TEST(log, "Current bucket: {}, will process file: {}",
                             bucket, object_info->getFileName());

                    return std::pair{object_info, bucket_holder_it->second->getBucketInfo()};
                }

                LOG_TEST(log, "Cache of bucket {} is empty", bucket);

                /// No more keys in bucket, remove it from cache.
                listed_keys_cache.erase(it);
            }
            else
            {
                LOG_TEST(log, "Cache of bucket {} is empty", bucket);
            }

            if (iterator_finished)
            {
                /// Bucket is fully processed - release the bucket.
                bucket_holder_it->second->release();
                bucket_holder_it->second.reset();
            }
        }
        /// If processing thread has already acquired some bucket
        /// and while listing s3 directory gets a key which is in a different bucket,
        /// it puts the key into listed_keys_cache to allow others to process it,
        /// because one processing thread can acquire only one bucket at a time.
        /// Once a thread is finished with its acquired bucket, it checks listed_keys_cache
        /// to see if there are keys from buckets not acquired by anyone.
        if (!bucket_holder_it->second)
        {
            for (auto it = listed_keys_cache.begin(); it != listed_keys_cache.end();)
            {
                auto & [bucket, bucket_info] = *it;
                auto & [bucket_keys, bucket_processor] = bucket_info;

                LOG_TEST(log, "Bucket: {}, cached keys: {}, processor: {}",
                         bucket, bucket_keys.size(), bucket_processor.has_value() ? toString(bucket_processor.value()) : "None");

                if (bucket_processor.has_value())
                {
                    LOG_TEST(log, "Bucket {} is already locked for processing by {} (keys: {})",
                             bucket, bucket_processor.value(), bucket_keys.size());
                    ++it;
                    continue;
                }

                if (bucket_keys.empty())
                {
                    /// No more keys in bucket, remove it from cache.
                    /// We still might add new keys to this bucket if !iterator_finished.
                    it = listed_keys_cache.erase(it);
                    continue;
                }

                bucket_holder_it->second = metadata->tryAcquireBucket(bucket, current_processor);
                if (!bucket_holder_it->second)
                {
                    LOG_TEST(log, "Bucket {} is already locked for processing (keys: {})",
                             bucket, bucket_keys.size());
                    ++it;
                    continue;
                }

                bucket_processor = current_processor;

                /// Take the key from the front, the order is important.
                auto object_info = bucket_keys.front();
                bucket_keys.pop_front();

                LOG_TEST(log, "Acquired bucket: {}, will process file: {}",
                         bucket, object_info->getFileName());

                return std::pair{object_info, bucket_holder_it->second->getBucketInfo()};
            }
        }

        if (iterator_finished)
        {
            LOG_TEST(log, "Reached the end of file iterator and nothing left in keys cache");
            return {};
        }

        auto object_info = glob_iterator->next(processor);
        if (object_info)
        {
            const auto bucket = metadata->getBucketForPath(object_info->relative_path);
            auto & bucket_cache = listed_keys_cache[bucket];

            LOG_TEST(log, "Found next file: {}, bucket: {}, current bucket: {}, cached_keys: {}",
                     object_info->getFileName(), bucket,
                     bucket_holder_it->second ? toString(bucket_holder_it->second->getBucket()) : "None",
                     bucket_cache.keys.size());

            if (bucket_holder_it->second)
            {
                if (bucket_holder_it->second->getBucket() != bucket)
                {
                    /// Acquired bucket differs from object's bucket,
                    /// put it into bucket's cache and continue.
                    bucket_cache.keys.emplace_back(object_info);
                    continue;
                }
                /// Bucket is already acquired, process the file.
                return std::pair{object_info, bucket_holder_it->second->getBucketInfo()};
            }
            else
            {
                bucket_holder_it->second = metadata->tryAcquireBucket(bucket, current_processor);
                if (bucket_holder_it->second)
                {
                    bucket_cache.processor = current_processor;
                    if (!bucket_cache.keys.empty())
                    {
                        /// We have to maintain ordering between keys,
                        /// so if some keys are already in cache - start with them.
                        bucket_cache.keys.emplace_back(object_info);
                        object_info = bucket_cache.keys.front();
                        bucket_cache.keys.pop_front();
                    }
                    return std::pair{object_info, bucket_holder_it->second->getBucketInfo()};
                }
                else
                {
                    LOG_TEST(log, "Bucket {} is already locked for processing", bucket);
                    bucket_cache.keys.emplace_back(object_info);
                    continue;
                }
            }
        }
        else
        {
            if (bucket_holder_it->second)
            {
                bucket_holder_it->second->release();
                bucket_holder_it->second.reset();
            }

            LOG_TEST(log, "Reached the end of file iterator");
            iterator_finished = true;

            if (listed_keys_cache.empty())
                return {};
            else
                continue;
        }
    }
}

StorageS3QueueSource::StorageS3QueueSource(
    String name_,
    size_t processor_id_,
    const Block & header_,
    std::unique_ptr<StorageObjectStorageSource> internal_source_,
    std::shared_ptr<S3QueueMetadata> files_metadata_,
    const S3QueueAction & action_,
    RemoveFileFunc remove_file_func_,
    const NamesAndTypesList & requested_virtual_columns_,
    ContextPtr context_,
    const std::atomic<bool> & shutdown_called_,
    const std::atomic<bool> & table_is_being_dropped_,
    std::shared_ptr<S3QueueLog> s3_queue_log_,
    const StorageID & storage_id_,
    LoggerPtr log_,
    size_t max_processed_files_before_commit_,
    size_t max_processed_rows_before_commit_,
    size_t max_processed_bytes_before_commit_,
    size_t max_processing_time_sec_before_commit_,
    bool commit_once_processed_)
    : ISource(header_)
    , WithContext(context_)
    , name(std::move(name_))
    , processor_id(processor_id_)
    , action(action_)
    , files_metadata(files_metadata_)
    , internal_source(std::move(internal_source_))
    , requested_virtual_columns(requested_virtual_columns_)
    , shutdown_called(shutdown_called_)
    , table_is_being_dropped(table_is_being_dropped_)
    , s3_queue_log(s3_queue_log_)
    , storage_id(storage_id_)
    , max_processed_files_before_commit(max_processed_files_before_commit_)
    , max_processed_rows_before_commit(max_processed_rows_before_commit_)
    , max_processed_bytes_before_commit(max_processed_bytes_before_commit_)
    , max_processing_time_sec_before_commit(max_processing_time_sec_before_commit_)
    , commit_once_processed(commit_once_processed_)
    , remove_file_func(remove_file_func_)
    , log(log_)
{
}

String StorageS3QueueSource::getName() const
{
    return name;
}

void StorageS3QueueSource::lazyInitialize(size_t processor)
{
    if (initialized)
        return;

    internal_source->lazyInitialize(processor);
    reader = std::move(internal_source->reader);
    if (reader)
        reader_future = std::move(internal_source->reader_future);
    initialized = true;
}

Chunk StorageS3QueueSource::generate()
{
    Chunk chunk;
    try
    {
        chunk = generateImpl();
    }
    catch (...)
    {
        if (commit_once_processed)
            commit(false, getCurrentExceptionMessage(true));

        throw;
    }

    if (!chunk && commit_once_processed)
    {
        commit(true);
    }
    return chunk;
}

Chunk StorageS3QueueSource::generateImpl()
{
    lazyInitialize(processor_id);

    while (true)
    {
        if (!reader)
        {
            LOG_TEST(log, "No reader");
            break;
        }

        const auto * object_info = dynamic_cast<const S3QueueObjectInfo *>(reader.getObjectInfo().get());
        auto file_metadata = object_info->file_metadata;
        auto file_status = file_metadata->getFileStatus();

        if (isCancelled())
        {
            reader->cancel();

            if (processed_rows_from_file)
            {
                try
                {
                    file_metadata->setFailed("Cancelled");
                }
                catch (...)
                {
                    LOG_ERROR(log, "Failed to set file {} as failed: {}",
                             object_info->relative_path, getCurrentExceptionMessage(true));
                }

                appendLogElement(reader.getObjectInfo()->getPath(), *file_status, processed_rows_from_file, false);
            }

            LOG_TEST(log, "Query is cancelled");
            break;
        }

        const auto & path = reader.getObjectInfo()->getPath();

        if (shutdown_called)
        {
            LOG_TEST(log, "Shutdown called");

            if (processed_rows_from_file == 0)
                break;

            if (table_is_being_dropped)
            {
                LOG_DEBUG(
                    log, "Table is being dropped, {} rows are already processed from {}, but file is not fully processed",
                    processed_rows_from_file, path);

                try
                {
                    file_metadata->setFailed("Table is dropped");
                }
                catch (...)
                {
                    LOG_ERROR(log, "Failed to set file {} as failed: {}",
                              object_info->relative_path, getCurrentExceptionMessage(true));
                }

                appendLogElement(path, *file_status, processed_rows_from_file, false);

                /// Leave the file half processed. Table is being dropped, so we do not care.
                break;
            }

            LOG_DEBUG(log, "Shutdown called, but file {} is partially processed ({} rows). "
                     "Will process the file fully and then shutdown",
                     path, processed_rows_from_file);
        }

        auto * prev_scope = CurrentThread::get().attachProfileCountersScope(&file_status->profile_counters);
        SCOPE_EXIT({ CurrentThread::get().attachProfileCountersScope(prev_scope); });
        /// FIXME:  if files are compressed, profile counters update does not work fully (s3 related counters are not saved). Why?

        try
        {
            auto timer = DB::CurrentThread::getProfileEvents().timer(ProfileEvents::S3QueuePullMicroseconds);

            Chunk chunk;
            if (reader->pull(chunk))
            {
                LOG_TEST(log, "Read {} rows from file: {}", chunk.getNumRows(), path);

                file_status->processed_rows += chunk.getNumRows();
                processed_rows_from_file += chunk.getNumRows();
                total_processed_rows += chunk.getNumRows();
                total_processed_bytes += chunk.bytes();

                VirtualColumnUtils::addRequestedPathFileAndSizeVirtualsToChunk(
                    chunk, requested_virtual_columns, path, reader.getObjectInfo()->metadata->size_bytes);
                return chunk;
            }
        }
        catch (...)
        {
            const auto message = getCurrentExceptionMessage(true);
            LOG_ERROR(log, "Got an error while pulling chunk. Will set file {} as failed. Error: {} ", path, message);

            appendLogElement(path, *file_status, processed_rows_from_file, false);

            failed_files.push_back(file_metadata);
            file_status->onFailed(getCurrentExceptionMessage(true));

            if (processed_rows_from_file == 0)
            {
                auto * file_iterator = dynamic_cast<FileIterator *>(internal_source->file_iterator.get());
                chassert(file_iterator);

                if (file_status->retries < file_metadata->getMaxTries())
                    file_iterator->returnForRetry(reader.getObjectInfo());

                /// If we did not process any rows from the failed file,
                /// commit all previously processed files,
                /// not to loose the work already done.
                return {};
            }

            throw;
        }

        appendLogElement(path, *file_status, processed_rows_from_file, true);

        file_status->onProcessed();
        file_status.reset();

        processed_rows_from_file = 0;
        processed_files.push_back(file_metadata);

        if (processed_files.size() == max_processed_files_before_commit)
        {
            LOG_TRACE(log, "Number of max processed files before commit reached "
                      "(rows: {}, bytes: {}, files: {})",
                      total_processed_rows, total_processed_bytes, processed_files.size());
            break;
        }

        bool rows_or_bytes_or_time_limit_reached = false;
        if (max_processed_rows_before_commit
            && total_processed_rows == max_processed_rows_before_commit)
        {
            LOG_TRACE(log, "Number of max processed rows before commit reached "
                      "(rows: {}, bytes: {}, files: {})",
                      total_processed_rows, total_processed_bytes, processed_files.size());

            rows_or_bytes_or_time_limit_reached = true;
        }
        else if (max_processed_bytes_before_commit
                 && total_processed_bytes == max_processed_bytes_before_commit)
        {
            LOG_TRACE(log, "Number of max processed bytes before commit reached "
                      "(rows: {}, bytes: {}, files: {})",
                      total_processed_rows, total_processed_bytes, processed_files.size());

            rows_or_bytes_or_time_limit_reached = true;
        }
        else if (max_processing_time_sec_before_commit
                 && total_stopwatch.elapsedSeconds() >= max_processing_time_sec_before_commit)
        {
            LOG_TRACE(log, "Max processing time before commit reached "
                      "(rows: {}, bytes: {}, files: {})",
                      total_processed_rows, total_processed_bytes, processed_files.size());

            rows_or_bytes_or_time_limit_reached = true;
        }

        if (rows_or_bytes_or_time_limit_reached)
        {
            if (!reader_future.valid())
                break;

            LOG_TRACE(log, "Rows or bytes limit reached, but we have one more file scheduled already, "
                      "will process it despite the limit");
        }

        if (shutdown_called)
        {
            LOG_TRACE(log, "Shutdown was called, stopping sync");
            break;
        }

        chassert(reader_future.valid());
        reader = reader_future.get();

        if (!reader)
        {
            LOG_TEST(log, "Reader finished");
            break;
        }

        file_status = files_metadata->getFileStatus(reader.getObjectInfo()->getPath());

        if (!rows_or_bytes_or_time_limit_reached && processed_files.size() + 1 < max_processed_files_before_commit)
        {
            /// Even if task is finished the thread may be not freed in pool.
            /// So wait until it will be freed before scheduling a new task.
            internal_source->create_reader_pool->wait();
            reader_future = internal_source->createReaderAsync(processor_id);
        }
    }

    return {};
}

void StorageS3QueueSource::commit(bool success, const std::string & exception)
{
    LOG_TEST(log, "Having {} files to set as {}, failed files: {}",
             processed_files.size(), success ? "Processed" : "Failed", failed_files.size());

    for (const auto & file_metadata : processed_files)
    {
        if (success)
        {
            file_metadata->setProcessed();
            applyActionAfterProcessing(file_metadata->getPath());
        }
        else
            file_metadata->setFailed(exception, /* reduce_retry_count */false);
    }

    for (const auto & file_metadata : failed_files)
    {
        /// `exception` from commit args is from insertion to storage.
        /// Here we do not used it as failed_files were not inserted into storage, but skipped.
        file_metadata->setFailed(file_metadata->getFileStatus()->getException(), /* reduce_retry_count */true);
    }
}

void StorageS3QueueSource::applyActionAfterProcessing(const String & path)
{
    switch (action)
    {
        case S3QueueAction::DELETE:
        {
            assert(remove_file_func);
            remove_file_func(path);
            break;
        }
        case S3QueueAction::KEEP:
            break;
    }
}

void StorageS3QueueSource::appendLogElement(
    const std::string & filename,
    S3QueueMetadata::FileStatus & file_status_,
    size_t processed_rows,
    bool processed)
{
    if (!s3_queue_log)
        return;

    S3QueueLogElement elem{};
    {
        elem = S3QueueLogElement
        {
            .event_time = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()),
            .database = storage_id.database_name,
            .table = storage_id.table_name,
            .uuid = toString(storage_id.uuid),
            .file_name = filename,
            .rows_processed = processed_rows,
            .status = processed ? S3QueueLogElement::S3QueueStatus::Processed : S3QueueLogElement::S3QueueStatus::Failed,
            .counters_snapshot = file_status_.profile_counters.getPartiallyAtomicSnapshot(),
            .processing_start_time = file_status_.processing_start_time,
            .processing_end_time = file_status_.processing_end_time,
            .exception = file_status_.getException(),
        };
    }
    s3_queue_log->add(std::move(elem));
}

}
