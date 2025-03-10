/*
 *  Copyright (c) 2021 NetEase Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/*
 * Project: curve
 * Created Date: 2021-05-19
 * Author: chenwei
 */

#ifndef CURVEFS_SRC_METASERVER_INODE_STORAGE_H_
#define CURVEFS_SRC_METASERVER_INODE_STORAGE_H_

#include <functional>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <list>
#include <string>
#include <memory>

#include "src/common/concurrent/rw_lock.h"
#include "curvefs/proto/metaserver.pb.h"
#include "curvefs/src/metaserver/storage/converter.h"
#include "curvefs/src/metaserver/storage/utils.h"
#include "curvefs/src/metaserver/storage/storage.h"

using curve::common::RWLock;
using curve::common::ReadLockGuard;
using curve::common::WriteLockGuard;
using ::curvefs::metaserver::storage::Status;
using ::curvefs::metaserver::storage::Iterator;
using ::curvefs::metaserver::storage::KVStorage;
using ::curvefs::metaserver::storage::StorageTransaction;
using ::curvefs::metaserver::storage::Hash;

namespace curvefs {
namespace metaserver {

using ::curvefs::metaserver::storage::Key4Inode;
using ::curvefs::metaserver::storage::Converter;
using S3ChunkInfoMap = google::protobuf::Map<uint64_t, S3ChunkInfoList>;

enum TABLE_TYPE : unsigned char {
    kTypeInode = 1,
    kTypeS3ChunkInfo = 2,
};

class InodeStorage {
 public:
    InodeStorage(std::shared_ptr<KVStorage> kvStorage,
                 const std::string& tablename);

    /**
     * @brief insert inode to storage
     * @param[in] inode: the inode want to insert
     * @return If inode exist, return INODE_EXIST; else insert and return OK
     */
    MetaStatusCode Insert(const Inode& inode);

    /**
     * @brief get inode from storage
     * @param[in] key: the key of inode want to get
     * @param[out] inode: the inode got
     * @return If inode not exist, return NOT_FOUND; else return OK
     */
    MetaStatusCode Get(const Key4Inode& key, Inode* inode);

    /**
     * @brief get inode attribute from storage
     * @param[in] key: the key of inode want to get
     * @param[out] attr: the inode attribute got
     * @return If inode not exist, return NOT_FOUND; else return OK
     */
    MetaStatusCode GetAttr(const Key4Inode& key, InodeAttr *attr);

    /**
     * @brief get inode extended attributes from storage
     * @param[in] key: the key of inode want to get
     * @param[out] attr: the inode extended attribute got
     * @return If inode not exist, return NOT_FOUND; else return OK
     */
    MetaStatusCode GetXAttr(const Key4Inode& key, XAttr *xattr);

    /**
     * @brief delete inode from storage
     * @param[in] key: the key of inode want to delete
     * @return If inode not exist, return NOT_FOUND; else return OK
     */
    MetaStatusCode Delete(const Key4Inode& key);

    /**
     * @brief update inode from storage
     * @param[in] inode: the inode want to update
     * @return If inode not exist, return NOT_FOUND; else replace and return OK
     */
    MetaStatusCode Update(const Inode& inode);

    MetaStatusCode ModifyInodeS3ChunkInfoList(uint32_t fsId,
                                              uint64_t inodeId,
                                              uint64_t chunkIndex,
                                              const S3ChunkInfoList* list2add,
                                              const S3ChunkInfoList* list2del);

    MetaStatusCode PaddingInodeS3ChunkInfo(int32_t fsId,
                                           uint64_t inodeId,
                                           S3ChunkInfoMap* m,
                                           uint64_t limit = 0);

    std::shared_ptr<Iterator> GetInodeS3ChunkInfoList(uint32_t fsId,
                                                      uint64_t inodeId);

    std::shared_ptr<Iterator> GetAllS3ChunkInfoList();

    std::shared_ptr<Iterator> GetAllInode();

    size_t Size();

    MetaStatusCode Clear();

    bool GetInodeIdList(std::list<uint64_t>* inodeIdList);

 private:
    MetaStatusCode AddS3ChunkInfoList(
        std::shared_ptr<StorageTransaction> txn,
        uint32_t fsId,
        uint64_t inodeId,
        uint64_t chunkIndex,
        const S3ChunkInfoList* list2add);

    MetaStatusCode DelS3ChunkInfoList(
        std::shared_ptr<StorageTransaction> txn,
        uint32_t fsId,
        uint64_t inodeId,
        uint64_t chunkIndex,
        const S3ChunkInfoList* list2del);

    std::string RealTablename(TABLE_TYPE type, std::string tablename) {
        std::ostringstream oss;
        oss << type << ":" << tablename;
        return oss.str();
    }

    static std::string InodeS3MetaSizeKey(uint32_t fsId, uint64_t inodeId) {
        std::ostringstream oss;
        oss << fsId << ":" << inodeId;
        return oss.str();
    }

    bool UpdateInodeS3MetaSize(uint32_t fsId, uint64_t inodeId,
                               uint64_t size4add, uint64_t size4del) {
        std::string key = InodeS3MetaSizeKey(fsId, inodeId);
        uint64_t size = inodeS3MetaSize_[key] + size4add;
        if (size < size4del) {
            return false;
        }
        inodeS3MetaSize_[key] = size - size4del;
        return true;
    }

    uint64_t GetInodeS3MetaSize(uint32_t fsId, uint64_t inodeId) {
        std::string key = InodeS3MetaSizeKey(fsId, inodeId);
        return inodeS3MetaSize_[key];
    }

    bool FindKey(const std::string& key) {
        return keySet_.find(key) != keySet_.end();
    }

    void InsertKey(const std::string& key) {
        keySet_.insert(key);
    }

    void EraseKey(const std::string& key) {
        keySet_.erase(key);
    }

 private:
    RWLock rwLock_;
    std::shared_ptr<KVStorage> kvStorage_;
    std::string table4inode_;
    std::string table4s3chunkinfo_;
    std::shared_ptr<Converter> conv_;
    std::unordered_set<std::string> keySet_;
    // key: Hash(inode), value: the number of inode's chunkinfo size
    std::unordered_map<std::string, uint64_t> inodeS3MetaSize_;
};

}  // namespace metaserver
}  // namespace curvefs

#endif  // CURVEFS_SRC_METASERVER_INODE_STORAGE_H_
