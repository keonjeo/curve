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

#ifndef CURVEFS_SRC_MDS_MDS_SERVICE_H_
#define CURVEFS_SRC_MDS_MDS_SERVICE_H_

#include <brpc/closure_guard.h>
#include <brpc/controller.h>

#include <memory>
#include <string>

#include "curvefs/proto/mds.pb.h"
#include "curvefs/src/mds/chunkid_allocator.h"
#include "curvefs/src/mds/fs_manager.h"
#include "curvefs/src/mds/common/mds_define.h"

namespace curvefs {
namespace mds {

class MdsServiceImpl : public MdsService {
 public:
    explicit MdsServiceImpl(
        std::shared_ptr<FsManager> fsManager,
        std::shared_ptr<ChunkIdAllocator> chunkIdAllocator) {
        fsManager_ = fsManager;
        chunkIdAllocator_ = chunkIdAllocator;
    }

    void CreateFs(::google::protobuf::RpcController* controller,
                  const CreateFsRequest* request,
                  CreateFsResponse* response,
                  ::google::protobuf::Closure* done);

    void MountFs(::google::protobuf::RpcController* controller,
                 const MountFsRequest* request,
                 MountFsResponse* response,
                 ::google::protobuf::Closure* done);

    void UmountFs(::google::protobuf::RpcController* controller,
                  const UmountFsRequest* request,
                  UmountFsResponse* response,
                  ::google::protobuf::Closure* done);

    void GetFsInfo(::google::protobuf::RpcController* controller,
                   const GetFsInfoRequest* request,
                   GetFsInfoResponse* response,
                   ::google::protobuf::Closure* done);

    void DeleteFs(::google::protobuf::RpcController* controller,
                  const DeleteFsRequest* request,
                  DeleteFsResponse* response,
                  ::google::protobuf::Closure* done);

    void AllocateS3Chunk(::google::protobuf::RpcController* controller,
                         const ::curvefs::mds::AllocateS3ChunkRequest* request,
                         ::curvefs::mds::AllocateS3ChunkResponse* response,
                         ::google::protobuf::Closure* done);

    void ListClusterFsInfo(
        ::google::protobuf::RpcController* controller,
        const ::curvefs::mds::ListClusterFsInfoRequest* request,
        ::curvefs::mds::ListClusterFsInfoResponse* response,
        ::google::protobuf::Closure* done);

    void RefreshSession(::google::protobuf::RpcController *controller,
                        const ::curvefs::mds::RefreshSessionRequest *request,
                        ::curvefs::mds::RefreshSessionResponse *response,
                        ::google::protobuf::Closure *done);

    void GetLatestTxId(::google::protobuf::RpcController* controller,
                       const GetLatestTxIdRequest* request,
                       GetLatestTxIdResponse* response,
                       ::google::protobuf::Closure* done);

    void CommitTx(::google::protobuf::RpcController* controller,
                  const CommitTxRequest* request,
                  CommitTxResponse* response,
                  ::google::protobuf::Closure* done);

 private:
    std::shared_ptr<FsManager> fsManager_;
    std::shared_ptr<ChunkIdAllocator> chunkIdAllocator_;
};
}  // namespace mds
}  // namespace curvefs

#endif  // CURVEFS_SRC_MDS_MDS_SERVICE_H_
