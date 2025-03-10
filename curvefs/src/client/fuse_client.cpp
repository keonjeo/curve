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
 * Created Date: Thur May 27 2021
 * Author: xuchaojie
 */

#include "curvefs/src/client/fuse_client.h"

#include <list>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#include <unordered_map>

#include "curvefs/proto/mds.pb.h"
#include "curvefs/src/client/fuse_common.h"
#include "curvefs/src/client/client_operator.h"
#include "curvefs/src/client/xattr_manager.h"
#include "src/common/net_common.h"
#include "src/common/dummyserver.h"
#include "src/client/client_common.h"

#define PORT_LIMIT 65535

using ::curvefs::common::S3Info;
using ::curvefs::common::Volume;
using ::curvefs::mds::topology::PartitionTxId;
using ::curvefs::mds::FSStatusCode_Name;
using ::curvefs::client::common::MAXXATTRLENGTH;

#define RETURN_IF_UNSUCCESS(action)                                            \
    do {                                                                       \
        rc = renameOp.action();                                                \
        if (rc != CURVEFS_ERROR::OK) {                                         \
            return rc;                                                         \
        }                                                                      \
    } while (0)


namespace curvefs {
namespace client {
namespace common {
DECLARE_bool(enableCto);
}  // namespace common
}  // namespace client
}  // namespace curvefs

namespace curvefs {
namespace client {

using common::MetaCacheOpt;
using rpcclient::ChannelManager;
using rpcclient::Cli2ClientImpl;
using rpcclient::MetaCache;
using common::FLAGS_enableCto;

CURVEFS_ERROR FuseClient::Init(const FuseClientOption &option) {
    option_ = option;

    mdsBase_ = new MDSBaseClient();
    FSStatusCode ret = mdsClient_->Init(option.mdsOpt, mdsBase_);
    if (ret != FSStatusCode::OK) {
        return CURVEFS_ERROR::INTERNAL;
    }

    auto cli2Client = std::make_shared<Cli2ClientImpl>();
    auto metaCache = std::make_shared<MetaCache>();
    metaCache->Init(option.metaCacheOpt, cli2Client, mdsClient_);
    auto channelManager = std::make_shared<ChannelManager<MetaserverID>>();

    leaseExecutor_ =
        std::make_shared<LeaseExecutor>(option.leaseOpt, metaCache, mdsClient_);

    xattrManager_ = std::make_shared<XattrManager>(inodeManager_,
        dentryManager_, option_.listDentryLimit, option_.listDentryThreads);

    uint32_t listenPort = 0;
    if (!curve::common::StartBrpcDummyserver(option.dummyServerStartPort,
                                             PORT_LIMIT, &listenPort)) {
        return CURVEFS_ERROR::INTERNAL;
    }

    std::string localIp;
    if (!curve::common::NetCommon::GetLocalIP(&localIp)) {
        LOG(ERROR) << "Get local ip failed!";
        return CURVEFS_ERROR::INTERNAL;
    }
    curve::client::ClientDummyServerInfo::GetInstance().SetPort(listenPort);
    curve::client::ClientDummyServerInfo::GetInstance().SetIP(localIp);

    MetaStatusCode ret2 =
        metaClient_->Init(option.excutorOpt, metaCache, channelManager);
    if (ret2 != MetaStatusCode::OK) {
        return CURVEFS_ERROR::INTERNAL;
    }

    CURVEFS_ERROR ret3 =
        inodeManager_->Init(option.iCacheLruSize, option.enableICacheMetrics);
    if (ret3 != CURVEFS_ERROR::OK) {
        return ret3;
    }

    ret3 =
        dentryManager_->Init(option.dCacheLruSize, option.enableDCacheMetrics);
    if (ret3 != CURVEFS_ERROR::OK) {
        return ret3;
    }

    if (!leaseExecutor_->Start()) {
        return CURVEFS_ERROR::INTERNAL;
    }

    return ret3;
}

void FuseClient::UnInit() {
    delete mdsBase_;
    mdsBase_ = nullptr;
}

CURVEFS_ERROR FuseClient::Run() {
    if (isStop_.exchange(false)) {
        flushThread_ = Thread(&FuseClient::FlushInodeLoop, this);
        LOG(INFO) << "Start fuse client flush thread ok.";
        return CURVEFS_ERROR::OK;
    }
    return CURVEFS_ERROR::INTERNAL;
}

void FuseClient::Fini() {
    if (!isStop_.exchange(true)) {
        LOG(INFO) << "stop fuse client flush thread ...";
        sleeper_.interrupt();
        flushThread_.join();
        xattrManager_->Stop();
    }
    LOG(INFO) << "stop fuse client flush thread ok.";
}

void FuseClient::FlushInodeLoop() {
    while (sleeper_.wait_for(std::chrono::seconds(option_.flushPeriodSec))) {
        FlushInode();
    }
}

CURVEFS_ERROR FuseClient::FuseOpInit(void *userdata,
                                     struct fuse_conn_info *conn) {
    struct MountOption *mOpts = (struct MountOption *)userdata;
    std::string mountPointStr =
        (mOpts->mountPoint == nullptr) ? "" : mOpts->mountPoint;
    std::string fsName = (mOpts->fsName == nullptr) ? "" : mOpts->fsName;

    int retVal = AddHostPortToMountPointStr(mountPointStr, &mountpoint_);
    if (retVal < 0) {
        LOG(ERROR) << "AddHostPortToMountPointStr failed, ret = " << retVal;
        return CURVEFS_ERROR::INTERNAL;
    }

    auto find = std::find(fsInfo_->mountpoints().begin(),
                          fsInfo_->mountpoints().end(), mountpoint_);
    if (find != fsInfo_->mountpoints().end()) {
        LOG(ERROR) << "MountFs found mountPoint exist";
        return CURVEFS_ERROR::MOUNT_POINT_EXIST;
    }

    auto ret = mdsClient_->MountFs(fsName, mountpoint_, fsInfo_.get());
    if (ret != FSStatusCode::OK && ret != FSStatusCode::MOUNT_POINT_EXIST) {
        LOG(ERROR) << "MountFs failed, FSStatusCode = " << ret
                   << ", FSStatusCode_Name = "
                   << FSStatusCode_Name(ret)
                   << ", fsName = " << fsName
                   << ", mountPoint = " << mountpoint_;
        return CURVEFS_ERROR::MOUNT_FAILED;
    }

    inodeManager_->SetFsId(fsInfo_->fsid());
    dentryManager_->SetFsId(fsInfo_->fsid());
    enableSumInDir_ = fsInfo_->enablesumindir() && !FLAGS_enableCto;
    LOG(INFO) << "Mount " << fsName << " on " << mountpoint_ << " success!"
              << " enableSumInDir = " << enableSumInDir_;

    fsMetric_ = std::make_shared<FSMetric>(fsName);

    init_ = true;

    return CURVEFS_ERROR::OK;
}

void FuseClient::FuseOpDestroy(void *userdata) {
    if (!init_) {
        return;
    }

    FlushAll();
    dirBuf_->DirBufferFreeAll();

    struct MountOption *mOpts = (struct MountOption *)userdata;
    std::string fsName = (mOpts->fsName == nullptr) ? "" : mOpts->fsName;
    std::string mountPointStr =
        (mOpts->mountPoint == nullptr) ? "" : mOpts->mountPoint;

    std::string mountPointWithHost;
    int retVal = AddHostPortToMountPointStr(mountPointStr, &mountPointWithHost);
    if (retVal < 0) {
        return;
    }
    LOG(INFO) << "Umount " << fsName << " on " << mountPointWithHost
              << " start";

    FSStatusCode ret = mdsClient_->UmountFs(fsName, mountPointWithHost);
    if (ret != FSStatusCode::OK && ret != FSStatusCode::MOUNT_POINT_NOT_EXIST) {
        LOG(ERROR) << "UmountFs failed, FSStatusCode = " << ret
                   << ", FSStatusCode_Name = "
                   << FSStatusCode_Name(ret)
                   << ", fsName = " << fsName
                   << ", mountPoint = " << mountPointWithHost;
        return;
    }

    LOG(INFO) << "Umount " << fsName << " on " << mountPointWithHost
              << " success!";
    return;
}

void FuseClient::GetDentryParamFromInode(
    const std::shared_ptr<InodeWrapper> &inodeWrapper_,
    fuse_entry_param *param) {
    memset(param, 0, sizeof(fuse_entry_param));
    param->ino = inodeWrapper_->GetInodeId();
    param->generation = 0;
    inodeWrapper_->GetInodeAttrLocked(&param->attr);
    param->attr_timeout = option_.attrTimeOut;
    param->entry_timeout = option_.entryTimeOut;
}

CURVEFS_ERROR FuseClient::FuseOpLookup(fuse_req_t req, fuse_ino_t parent,
                                       const char *name, fuse_entry_param *e) {
    VLOG(1) << "FuseOpLookup parent: " << parent
            << ", name: " << name;
    if (strlen(name) > option_.maxNameLength) {
        return CURVEFS_ERROR::NAMETOOLONG;
    }
    Dentry dentry;
    CURVEFS_ERROR ret = dentryManager_->GetDentry(parent, name, &dentry);
    if (ret != CURVEFS_ERROR::OK) {
        if (ret != CURVEFS_ERROR::NOTEXIST) {
            LOG(WARNING) << "dentryManager_ get dentry fail, ret = " << ret
                         << ", parent inodeid = " << parent
                         << ", name = " << name;
        }
        return ret;
    }
    std::shared_ptr<InodeWrapper> inodeWrapper;
    fuse_ino_t ino = dentry.inodeid();
    ret = inodeManager_->GetInode(ino, inodeWrapper);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager get inode fail, ret = " << ret
                   << ", inodeid = " << ino;
        return ret;
    }

    GetDentryParamFromInode(inodeWrapper, e);
    return ret;
}

CURVEFS_ERROR FuseClient::FuseOpOpen(fuse_req_t req, fuse_ino_t ino,
                                     struct fuse_file_info *fi) {
    LOG(INFO) << "FuseOpOpen, ino: " << ino;
    std::shared_ptr<InodeWrapper> inodeWrapper;
    CURVEFS_ERROR ret = inodeManager_->GetInode(ino, inodeWrapper);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager get inode fail, ret = " << ret
                   << ", inodeid = " << ino;
        return ret;
    }

    ::curve::common::UniqueLock lgGuard = inodeWrapper->GetUniqueLock();

    ret = inodeWrapper->Open();
    if (ret != CURVEFS_ERROR::OK) {
        return ret;
    }

    if (fi->flags & O_TRUNC) {
        if (fi->flags & O_WRONLY || fi->flags & O_RDWR) {
            Inode *inode = inodeWrapper->GetMutableInodeUnlocked();
            uint64_t length = inode->length();
            CURVEFS_ERROR tRet = Truncate(inode, 0);
            if (tRet != CURVEFS_ERROR::OK) {
                LOG(ERROR) << "truncate file fail, ret = " << ret
                           << ", inodeid = " << ino;
                return CURVEFS_ERROR::INTERNAL;
            }
            inode->set_length(0);
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            inode->set_ctime(now.tv_sec);
            inode->set_ctime_ns(now.tv_nsec);
            inode->set_mtime(now.tv_sec);
            inode->set_mtime_ns(now.tv_nsec);
            ret = inodeWrapper->Sync();
            if (ret != CURVEFS_ERROR::OK) {
                return ret;
            }

            if (enableSumInDir_ && length != 0) {
                // update parent summary info
                XAttr xattr;
                xattr.mutable_xattrinfos()->insert({XATTRFBYTES,
                    std::to_string(length)});
                for (const auto &it : inode->parent()) {
                    auto tret = xattrManager_->UpdateParentInodeXattr(
                        it, xattr, false);
                    if (tret != CURVEFS_ERROR::OK) {
                        LOG(ERROR) << "UpdateParentInodeXattr failed,"
                                   << " inodeId = " << it
                                   << ", xattr = " << xattr.DebugString();
                    }
                }
            }
        } else {
            return CURVEFS_ERROR::NOPERMISSION;
        }
    }

    return ret;
}

CURVEFS_ERROR FuseClient::MakeNode(fuse_req_t req, fuse_ino_t parent,
                                   const char *name, mode_t mode,
                                   FsFileType type, dev_t rdev,
                                   fuse_entry_param *e) {
    if (strlen(name) > option_.maxNameLength) {
        return CURVEFS_ERROR::NAMETOOLONG;
    }
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    InodeParam param;
    param.fsId = fsInfo_->fsid();
    if (FsFileType::TYPE_DIRECTORY == type) {
        param.length = 4096;
    } else {
        param.length = 0;
    }
    param.uid = ctx->uid;
    param.gid = ctx->gid;
    param.mode = mode;
    param.type = type;
    param.rdev = rdev;
    param.parent = parent;

    std::shared_ptr<InodeWrapper> inodeWrapper;
    CURVEFS_ERROR ret = inodeManager_->CreateInode(param, inodeWrapper);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager CreateInode fail, ret = " << ret
                   << ", parent = " << parent << ", name = " << name
                   << ", mode = " << mode;
        return ret;
    }

    VLOG(6) << "inodeManager CreateInode success"
            << ", parent = " << parent << ", name = " << name
            << ", mode = " << mode
            << ", inode id = " << inodeWrapper->GetInodeId();

    Dentry dentry;
    dentry.set_fsid(fsInfo_->fsid());
    dentry.set_inodeid(inodeWrapper->GetInodeId());
    dentry.set_parentinodeid(parent);
    dentry.set_name(name);
    dentry.set_type(inodeWrapper->GetType());
    if (type == FsFileType::TYPE_FILE || type == FsFileType::TYPE_S3) {
        dentry.set_flag(DentryFlag::TYPE_FILE_FLAG);
    }

    ret = dentryManager_->CreateDentry(dentry);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "dentryManager_ CreateDentry fail, ret = " << ret
                   << ", parent = " << parent << ", name = " << name
                   << ", mode = " << mode;

        CURVEFS_ERROR ret2 =
            inodeManager_->DeleteInode(inodeWrapper->GetInodeId());
        if (ret2 != CURVEFS_ERROR::OK) {
            LOG(ERROR) << "Also delete inode failed, ret = " << ret2
                       << ", inodeid = " << inodeWrapper->GetInodeId();
        }
        return ret;
    }

    std::shared_ptr<InodeWrapper> parentInodeWrapper;
    ret = inodeManager_->GetInode(parent, parentInodeWrapper);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager get inode fail, ret = " << ret
                   << ", inodeid = " << parent;
        return ret;
    }
    ret = parentInodeWrapper->IncreaseNLink();
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager IncreaseNLink fail, ret = " << ret
                   << ", inodeid = " << parent;
        return ret;
    }

    VLOG(6) << "dentryManager_ CreateDentry success"
            << ", parent = " << parent << ", name = " << name
            << ", mode = " << mode;

    if (enableSumInDir_) {
        // update parent summary info
        XAttr xattr;
        xattr.mutable_xattrinfos()->insert({XATTRENTRIES, "1"});
        if (type == FsFileType::TYPE_DIRECTORY) {
            xattr.mutable_xattrinfos()->insert({XATTRSUBDIRS, "1"});
        } else {
            xattr.mutable_xattrinfos()->insert({XATTRFILES, "1"});
        }
        xattr.mutable_xattrinfos()->insert({XATTRFBYTES,
            std::to_string(inodeWrapper->GetLength())});
        auto tret = xattrManager_->UpdateParentInodeXattr(parent, xattr, true);
        if (tret != CURVEFS_ERROR::OK) {
            LOG(ERROR) << "UpdateParentInodeXattr failed,"
                       << " inodeId = " << parent
                       << ", xattr = " << xattr.DebugString();
        }
    }

    GetDentryParamFromInode(inodeWrapper, e);
    return ret;
}

CURVEFS_ERROR FuseClient::FuseOpMkDir(fuse_req_t req, fuse_ino_t parent,
                                      const char *name, mode_t mode,
                                      fuse_entry_param *e) {
    LOG(INFO) << "FuseOpMkDir, parent: " << parent
              << ", name: " << name
              << ", mode: " << mode;
    return MakeNode(req, parent, name, S_IFDIR | mode,
                    FsFileType::TYPE_DIRECTORY, 0, e);
}

CURVEFS_ERROR FuseClient::FuseOpUnlink(fuse_req_t req, fuse_ino_t parent,
                                       const char *name) {
    LOG(INFO) << "FuseOpUnlink, parent: " << parent
              << ", name: " << name;
    return RemoveNode(req, parent, name, false);
}

CURVEFS_ERROR FuseClient::FuseOpRmDir(fuse_req_t req, fuse_ino_t parent,
                                      const char *name) {
    LOG(INFO) << "FuseOpRmDir, parent: " << parent
              << ", name: " << name;
    return RemoveNode(req, parent, name, true);
}

CURVEFS_ERROR FuseClient::RemoveNode(fuse_req_t req, fuse_ino_t parent,
                                     const char *name, bool isDir) {
    if (strlen(name) > option_.maxNameLength) {
        return CURVEFS_ERROR::NAMETOOLONG;
    }
    Dentry dentry;
    CURVEFS_ERROR ret = dentryManager_->GetDentry(parent, name, &dentry);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(WARNING) << "dentryManager_ GetDentry fail, ret = " << ret
                     << ", parent = " << parent << ", name = " << name;
        return ret;
    }

    uint64_t ino = dentry.inodeid();

    if (isDir) {
        std::list<Dentry> dentryList;
        auto limit = option_.listDentryLimit;
        ret = dentryManager_->ListDentry(ino, &dentryList, limit);
        if (ret != CURVEFS_ERROR::OK) {
            LOG(ERROR) << "dentryManager_ ListDentry fail, ret = " << ret
                       << ", parent = " << ino;
            return ret;
        }
        if (!dentryList.empty()) {
            LOG(ERROR) << "rmdir not empty";
            return CURVEFS_ERROR::NOTEMPTY;
        }
    }

    ret = dentryManager_->DeleteDentry(parent, name);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "dentryManager_ DeleteDentry fail, ret = " << ret
                   << ", parent = " << parent << ", name = " << name;
        return ret;
    }

    std::shared_ptr<InodeWrapper> parentInodeWrapper;
    ret = inodeManager_->GetInode(parent, parentInodeWrapper);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager get inode fail, ret = " << ret
                   << ", inodeid = " << parent;
        return ret;
    }
    ret = parentInodeWrapper->DecreaseNLink();
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager DecreaseNLink fail, ret = " << ret
                   << ", inodeid = " << parent;
        return ret;
    }

    std::shared_ptr<InodeWrapper> inodeWrapper;
    ret = inodeManager_->GetInode(ino, inodeWrapper);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager get inode fail, ret = " << ret
                   << ", inodeid = " << ino;
        return ret;
    }

    ret = inodeWrapper->UnLinkLocked(parent);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "UnLink failed, ret = " << ret << ", inodeid = " << ino
                   << ", parent = " << parent << ", name = " << name;
    }

    if (enableSumInDir_) {
        // update parent summary info
        XAttr xattr;
        xattr.mutable_xattrinfos()->insert({XATTRENTRIES, "1"});
        if (isDir) {
            xattr.mutable_xattrinfos()->insert({XATTRSUBDIRS, "1"});
        } else {
            xattr.mutable_xattrinfos()->insert({XATTRFILES, "1"});
        }
        xattr.mutable_xattrinfos()->insert({XATTRFBYTES,
            std::to_string(inodeWrapper->GetLength())});
        auto tret = xattrManager_->UpdateParentInodeXattr(parent, xattr, false);
        if (tret != CURVEFS_ERROR::OK) {
            LOG(ERROR) << "UpdateParentInodeXattr failed,"
                       << " inodeId = " << parent
                       << ", xattr = " << xattr.DebugString();
        }
    }

    inodeManager_->ClearInodeCache(ino);
    return ret;
}

CURVEFS_ERROR FuseClient::FuseOpOpenDir(fuse_req_t req, fuse_ino_t ino,
                                        struct fuse_file_info *fi) {
    std::shared_ptr<InodeWrapper> inodeWrapper;
    CURVEFS_ERROR ret = inodeManager_->GetInode(ino, inodeWrapper);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager get inode fail, ret = " << ret
                   << ", inodeid = " << ino;
        return ret;
    }

    ::curve::common::UniqueLock lgGuard = inodeWrapper->GetUniqueLock();

    uint64_t dindex = dirBuf_->DirBufferNew();
    fi->fh = dindex;
    VLOG(1) << "FuseOpOpenDir, ino: " << ino << ", dindex: " << dindex;

    return ret;
}

CURVEFS_ERROR FuseClient::FuseOpReleaseDir(fuse_req_t req, fuse_ino_t ino,
                                           struct fuse_file_info *fi) {
    uint64_t dindex = fi->fh;
    VLOG(1) << "FuseOpReleaseDir, ino: " << ino << ", dindex: " << dindex;
    dirBuf_->DirBufferRelease(dindex);
    return CURVEFS_ERROR::OK;
}

static void dirbuf_add(fuse_req_t req, struct DirBufferHead *b,
                       const Dentry &dentry) {
    struct stat stbuf;
    size_t oldsize = b->size;
    b->size += fuse_add_direntry(req, NULL, 0, dentry.name().c_str(), NULL, 0);
    b->p = static_cast<char *>(realloc(b->p, b->size));
    memset(&stbuf, 0, sizeof(stbuf));
    stbuf.st_ino = dentry.inodeid();
    fuse_add_direntry(req, b->p + oldsize, b->size - oldsize,
                      dentry.name().c_str(), &stbuf, b->size);
}

CURVEFS_ERROR FuseClient::FuseOpReadDir(fuse_req_t req, fuse_ino_t ino,
                                        size_t size, off_t off,
                                        struct fuse_file_info *fi,
                                        char **buffer, size_t *rSize) {
    VLOG(6) << "FuseOpReadDir ino: " << ino << ", size: " << size
            << ", off = " << off;
    std::shared_ptr<InodeWrapper> inodeWrapper;
    CURVEFS_ERROR ret = inodeManager_->GetInode(ino, inodeWrapper);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager get inode fail, ret = " << ret
                   << ", inodeid = " << ino;
        return ret;
    }

    ::curve::common::UniqueLock lgGuard = inodeWrapper->GetUniqueLock();

    uint64_t dindex = fi->fh;
    DirBufferHead *bufHead = dirBuf_->DirBufferGet(dindex);
    if (!bufHead->wasRead) {
        std::list<Dentry> dentryList;
        auto limit = option_.listDentryLimit;
        ret = dentryManager_->ListDentry(ino, &dentryList, limit);
        if (ret != CURVEFS_ERROR::OK) {
            LOG(ERROR) << "dentryManager_ ListDentry fail, ret = " << ret
                       << ", parent = " << ino;
            return ret;
        }
        for (const auto &dentry : dentryList) {
            dirbuf_add(req, bufHead, dentry);
        }
        bufHead->wasRead = true;
    }
    if (off < bufHead->size) {
        *buffer = bufHead->p + off;
        *rSize = std::min(bufHead->size - off, size);
    } else {
        *buffer = nullptr;
        *rSize = 0;
    }
    return ret;
}

CURVEFS_ERROR FuseClient::FuseOpRename(fuse_req_t req, fuse_ino_t parent,
                                       const char *name, fuse_ino_t newparent,
                                       const char *newname) {
    LOG(INFO) << "FuseOpRename from (" << parent << ", " << name << ") to ("
              << newparent << ", " << newname << ")";
    if (strlen(name) > option_.maxNameLength ||
        strlen(newname) > option_.maxNameLength) {
        return CURVEFS_ERROR::NAMETOOLONG;
    }

    auto renameOp =
        RenameOperator(fsInfo_->fsid(), fsInfo_->fsname(),
                       parent, name, newparent, newname,
                       dentryManager_, inodeManager_, metaClient_, mdsClient_,
                       option_.enableMultiMountPointRename);

    curve::common::LockGuard lg(renameMutex_);
    CURVEFS_ERROR rc = CURVEFS_ERROR::OK;
    RETURN_IF_UNSUCCESS(GetTxId);
    RETURN_IF_UNSUCCESS(Precheck);
    RETURN_IF_UNSUCCESS(RecordOldInodeInfo);
    RETURN_IF_UNSUCCESS(LinkDestParentInode);
    RETURN_IF_UNSUCCESS(PrepareTx);
    RETURN_IF_UNSUCCESS(CommitTx);
    renameOp.UnlinkSrcParentInode();
    renameOp.UnlinkOldInode();
    renameOp.UpdateInodeParent();
    renameOp.UpdateCache();

    if (enableSumInDir_) {
        xattrManager_->UpdateParentXattrAfterRename(
            parent, newparent, newname, &renameOp);
    }

    return rc;
}

CURVEFS_ERROR FuseClient::FuseOpGetAttr(fuse_req_t req, fuse_ino_t ino,
                                        struct fuse_file_info *fi,
                                        struct stat *attr) {
    std::shared_ptr<InodeWrapper> inodeWrapper;
    CURVEFS_ERROR ret = inodeManager_->GetInode(ino, inodeWrapper);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager get inode fail, ret = " << ret
                   << ", inodeid = " << ino;
        return ret;
    }
    inodeWrapper->GetInodeAttrLocked(attr);
    return ret;
}

CURVEFS_ERROR FuseClient::FuseOpSetAttr(fuse_req_t req, fuse_ino_t ino,
                                        struct stat *attr, int to_set,
                                        struct fuse_file_info *fi,
                                        struct stat *attrOut) {
    LOG(INFO) << "FuseOpSetAttr to_set: " << to_set
              << ", ino: " << ino
              << ", attr: " << *attr;
    std::shared_ptr<InodeWrapper> inodeWrapper;
    CURVEFS_ERROR ret = inodeManager_->GetInode(ino, inodeWrapper);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager get inode fail, ret = " << ret
                   << ", inodeid = " << ino;
        return ret;
    }

    ::curve::common::UniqueLock lgGuard = inodeWrapper->GetUniqueLock();
    Inode *inode = inodeWrapper->GetMutableInodeUnlocked();

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    if (to_set & FUSE_SET_ATTR_MODE) {
        inode->set_mode(attr->st_mode);
    }
    if (to_set & FUSE_SET_ATTR_UID) {
        inode->set_uid(attr->st_uid);
    }
    if (to_set & FUSE_SET_ATTR_GID) {
        inode->set_gid(attr->st_gid);
    }
    if (to_set & FUSE_SET_ATTR_ATIME) {
        inode->set_atime(attr->st_atim.tv_sec);
        inode->set_atime_ns(attr->st_atim.tv_nsec);
    }
    if (to_set & FUSE_SET_ATTR_ATIME_NOW) {
        inode->set_atime(now.tv_sec);
        inode->set_atime_ns(now.tv_nsec);
    }
    if (to_set & FUSE_SET_ATTR_MTIME) {
        inode->set_mtime(attr->st_mtim.tv_sec);
        inode->set_mtime_ns(attr->st_mtim.tv_nsec);
    }
    if (to_set & FUSE_SET_ATTR_MTIME_NOW) {
        inode->set_mtime(now.tv_sec);
        inode->set_mtime_ns(now.tv_nsec);
    }
    if (to_set & FUSE_SET_ATTR_CTIME) {
        inode->set_ctime(attr->st_ctim.tv_sec);
        inode->set_ctime_ns(attr->st_ctim.tv_nsec);
    } else {
        inode->set_ctime(now.tv_sec);
        inode->set_ctime_ns(now.tv_nsec);
    }
    if (to_set & FUSE_SET_ATTR_SIZE) {
        int64_t changeSize =  attr->st_size - inode->length();
        CURVEFS_ERROR tRet = Truncate(inode, attr->st_size);
        if (tRet != CURVEFS_ERROR::OK) {
            LOG(ERROR) << "truncate file fail, ret = " << ret
                       << ", inodeid = " << ino;
            return tRet;
        }
        inode->set_length(attr->st_size);
        ret = inodeWrapper->Sync();
        if (ret != CURVEFS_ERROR::OK) {
            return ret;
        }
        inodeWrapper->GetInodeAttrUnLocked(attrOut);

        if (enableSumInDir_ && changeSize != 0) {
            // update parent summary info
            XAttr xattr;
            xattr.mutable_xattrinfos()->insert({XATTRFBYTES,
                std::to_string(std::abs(changeSize))});
            bool direction = changeSize > 0;
            for (const auto &it : inode->parent()) {
                auto tret = xattrManager_->UpdateParentInodeXattr(
                    it, xattr, direction);
                if (tret != CURVEFS_ERROR::OK) {
                    LOG(ERROR) << "UpdateParentInodeXattr failed,"
                               << " inodeId = " << it
                               << ", xattr = " << xattr.DebugString();
                }
            }
        }
        return ret;
    }
    ret = inodeWrapper->SyncAttr();
    if (ret != CURVEFS_ERROR::OK) {
        return ret;
    }
    inodeWrapper->GetInodeAttrUnLocked(attrOut);
    return ret;
}

bool IsSummaryInfo(const char *name) {
    return std::strstr(name, SUMMARYPREFIX);
}

bool IsOneLayer(const char *name) {
    if (std::strcmp(name, XATTRFILES) == 0 ||
        std::strcmp(name, XATTRSUBDIRS) == 0 ||
        std::strcmp(name, XATTRENTRIES) == 0 ||
        std::strcmp(name, XATTRFBYTES) == 0) {
        return true;
    }
    return false;
}

CURVEFS_ERROR FuseClient::FuseOpGetXattr(fuse_req_t req, fuse_ino_t ino,
                                         const char* name, void* value,
                                         size_t size) {
    VLOG(9) << "FuseOpGetXattr, ino: " << ino
            << ", name: " << name << ", size = " << size;
    std::shared_ptr<InodeWrapper> inodeWrapper;
    CURVEFS_ERROR ret = inodeManager_->GetInode(ino, inodeWrapper);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager get inode fail, ret = " << ret
                   << ", inodeid = " << ino;
        return ret;
    }

    ::curve::common::UniqueLock lgGuard = inodeWrapper->GetUniqueLock();
    std::string xValue;

    // get summary info
    if (IsSummaryInfo(name) &&
        inodeWrapper->GetType() == FsFileType::TYPE_DIRECTORY) {
        Inode inode = inodeWrapper->GetInodeUnlocked();
        // not enable record summary info in dir xattr,
        // need recursive computation all files
        if (!enableSumInDir_) {
            if (IsOneLayer(name)) {
                ret = xattrManager_->CalOneLayerSumInfo(&inode);
            } else {
                ret = xattrManager_->CalAllLayerSumInfo(&inode);
            }
        } else {
            if (IsOneLayer(name)) {
                ret = xattrManager_->FastCalOneLayerSumInfo(&inode);
            } else {
                ret = xattrManager_->FastCalAllLayerSumInfo(&inode);
            }
        }

        if (CURVEFS_ERROR::OK != ret) {
            return ret;
        }
        VLOG(1) << "After calculate summary info:\n"
                << inode.DebugString();
        auto it = inode.xattr().find(name);
        if (it != inode.xattr().end()) {
            xValue = it->second;
        }
    } else {
        inodeWrapper->GetXattrUnLocked(name, &xValue);
    }

    ret = CURVEFS_ERROR::NODATA;
    if (xValue.length() > 0) {
        if ((size == 0 && xValue.length() <= MAXXATTRLENGTH) ||
            (size >= xValue.length() && size <= MAXXATTRLENGTH)) {
            memcpy(value, xValue.c_str(), xValue.length());
            ret = CURVEFS_ERROR::OK;
        } else {
            ret = CURVEFS_ERROR::OUT_OF_RANGE;
        }
    }
    return ret;
}

CURVEFS_ERROR FuseClient::FuseOpListXattr(fuse_req_t req, fuse_ino_t ino,
                            char *value, size_t size, size_t *realSize) {
    LOG(INFO) << "FuseOpListXattr, ino: " << ino
              << ", size = " << size;
    std::shared_ptr<InodeWrapper> inodeWrapper;
    CURVEFS_ERROR ret = inodeManager_->GetInode(ino, inodeWrapper);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager get inode fail, ret = " << ret
                   << ", inodeid = " << ino;
        return ret;
    }

    Inode inode = inodeWrapper->GetInodeLocked();
    // get xattr key
    for (const auto &it : inode.xattr()) {
        // +1 because, the format is key\0key\0
        *realSize += it.first.length() + 1;
    }

    // add summary xattr key
    if (inode.type() == FsFileType::TYPE_DIRECTORY) {
        *realSize += strlen(XATTRRFILES) + 1;
        *realSize += strlen(XATTRRSUBDIRS) + 1;
        *realSize += strlen(XATTRRENTRIES) + 1;
        *realSize += strlen(XATTRRFBYTES) + 1;
    }

    if (size == 0) {
        return CURVEFS_ERROR::OK;
    } else if (size >= *realSize) {
        for (const auto &it : inode.xattr()) {
            auto tsize = it.first.length() + 1;
            memcpy(value, it.first.c_str(), tsize);
            value += tsize;
        }
        if (inode.type() == FsFileType::TYPE_DIRECTORY) {
            memcpy(value, XATTRRFILES, strlen(XATTRRFILES) + 1);
            value += strlen(XATTRRFILES) + 1;
            memcpy(value, XATTRRSUBDIRS, strlen(XATTRRSUBDIRS) + 1);
            value += strlen(XATTRRSUBDIRS) + 1;
            memcpy(value, XATTRRENTRIES, strlen(XATTRRENTRIES) + 1);
            value += strlen(XATTRRENTRIES) + 1;
            memcpy(value, XATTRRFBYTES, strlen(XATTRRFBYTES) + 1);
            value += strlen(XATTRRFBYTES) + 1;
        }
        return CURVEFS_ERROR::OK;
    }
    return CURVEFS_ERROR::OUT_OF_RANGE;
}

CURVEFS_ERROR FuseClient::FuseOpSymlink(fuse_req_t req, const char *link,
                                        fuse_ino_t parent, const char *name,
                                        fuse_entry_param *e) {
    LOG(INFO) << "FuseOpSymlink, link: " << link
              << ", parent: " << parent
              << ", name: " << name;
    if (strlen(name) > option_.maxNameLength) {
        return CURVEFS_ERROR::NAMETOOLONG;
    }
    const struct fuse_ctx *ctx = fuse_req_ctx(req);
    InodeParam param;
    param.fsId = fsInfo_->fsid();
    param.length = std::strlen(link);
    param.uid = ctx->uid;
    param.gid = ctx->gid;
    param.mode = S_IFLNK | 0777;
    param.type = FsFileType::TYPE_SYM_LINK;
    param.symlink = link;
    param.parent = parent;

    std::shared_ptr<InodeWrapper> inodeWrapper;
    CURVEFS_ERROR ret = inodeManager_->CreateInode(param, inodeWrapper);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager CreateInode fail, ret = " << ret
                   << ", parent = " << parent << ", name = " << name
                   << ", mode = " << param.mode;
        return ret;
    }
    Dentry dentry;
    dentry.set_fsid(fsInfo_->fsid());
    dentry.set_inodeid(inodeWrapper->GetInodeId());
    dentry.set_parentinodeid(parent);
    dentry.set_name(name);
    dentry.set_type(inodeWrapper->GetType());
    ret = dentryManager_->CreateDentry(dentry);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "dentryManager_ CreateDentry fail, ret = " << ret
                   << ", parent = " << parent << ", name = " << name
                   << ", mode = " << param.mode;

        CURVEFS_ERROR ret2 =
            inodeManager_->DeleteInode(inodeWrapper->GetInodeId());
        if (ret2 != CURVEFS_ERROR::OK) {
            LOG(ERROR) << "Also delete inode failed, ret = " << ret2
                       << ", inodeid = " << inodeWrapper->GetInodeId();
        }
        return ret;
    }

    std::shared_ptr<InodeWrapper> parentInodeWrapper;
    ret = inodeManager_->GetInode(parent, parentInodeWrapper);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager get inode fail, ret = " << ret
                   << ", inodeid = " << parent;
        return ret;
    }
    ret = parentInodeWrapper->IncreaseNLink();
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager IncreaseNLink fail, ret = " << ret
                   << ", inodeid = " << parent;
        return ret;
    }

    if (enableSumInDir_) {
        // update parent summary info
        XAttr xattr;
        xattr.mutable_xattrinfos()->insert({XATTRENTRIES, "1"});
        xattr.mutable_xattrinfos()->insert({XATTRFILES, "1"});
        xattr.mutable_xattrinfos()->insert({XATTRFBYTES,
            std::to_string(inodeWrapper->GetLength())});
        auto tret = xattrManager_->UpdateParentInodeXattr(parent, xattr, true);
        if (tret != CURVEFS_ERROR::OK) {
            LOG(ERROR) << "UpdateParentInodeXattr failed,"
                       << " inodeId = " << parent
                       << ", xattr = " << xattr.DebugString();
        }
    }

    GetDentryParamFromInode(inodeWrapper, e);
    return ret;
}

CURVEFS_ERROR FuseClient::FuseOpLink(fuse_req_t req, fuse_ino_t ino,
                                     fuse_ino_t newparent, const char *newname,
                                     fuse_entry_param *e) {
    LOG(INFO) << "FuseOpLink, ino: " << ino
              << ", newparent: " << newparent
              << ", newname: " << newname;
    if (strlen(newname) > option_.maxNameLength) {
        return CURVEFS_ERROR::NAMETOOLONG;
    }
    std::shared_ptr<InodeWrapper> inodeWrapper;
    CURVEFS_ERROR ret = inodeManager_->GetInode(ino, inodeWrapper);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager get inode fail, ret = " << ret
                   << ", inodeid = " << ino;
        return ret;
    }
    ret = inodeWrapper->LinkLocked(newparent);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "Link Inode fail, ret = " << ret << ", inodeid = " << ino
                   << ", newparent = " << newparent
                   << ", newname = " << newname;
        return ret;
    }
    Dentry dentry;
    dentry.set_fsid(fsInfo_->fsid());
    dentry.set_inodeid(inodeWrapper->GetInodeId());
    dentry.set_parentinodeid(newparent);
    dentry.set_name(newname);
    dentry.set_type(inodeWrapper->GetType());
    ret = dentryManager_->CreateDentry(dentry);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "dentryManager_ CreateDentry fail, ret = " << ret
                   << ", parent = " << newparent << ", name = " << newname;

        CURVEFS_ERROR ret2 = inodeWrapper->UnLinkLocked(newparent);
        if (ret2 != CURVEFS_ERROR::OK) {
            LOG(ERROR) << "Also unlink inode failed, ret = " << ret2
                       << ", inodeid = " << inodeWrapper->GetInodeId();
        }
        return ret;
    }

    std::shared_ptr<InodeWrapper> parentInodeWrapper;
    ret = inodeManager_->GetInode(newparent, parentInodeWrapper);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager get inode fail, ret = " << ret
                   << ", inodeid = " << newparent;
        return ret;
    }
    ret = parentInodeWrapper->IncreaseNLink();
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager IncreaseNLink fail, ret = " << ret
                   << ", inodeid = " << newparent;
        return ret;
    }

    if (enableSumInDir_) {
        // update parent summary info
        XAttr xattr;
        xattr.mutable_xattrinfos()->insert({XATTRENTRIES, "1"});
        xattr.mutable_xattrinfos()->insert({XATTRFILES, "1"});
        xattr.mutable_xattrinfos()->insert({XATTRFBYTES,
            std::to_string(inodeWrapper->GetLength())});
        auto tret = xattrManager_->UpdateParentInodeXattr(
            newparent, xattr, true);
        if (tret != CURVEFS_ERROR::OK) {
            LOG(ERROR) << "UpdateParentInodeXattr failed,"
                       << " inodeId = " << newparent
                       << ", xattr = " << xattr.DebugString();
        }
    }

    GetDentryParamFromInode(inodeWrapper, e);
    return ret;
}

CURVEFS_ERROR FuseClient::FuseOpReadLink(fuse_req_t req, fuse_ino_t ino,
                                         std::string *linkStr) {
    LOG(INFO) << "FuseOpReadLink, ino: " << ino
              << ", linkStr: " << linkStr;
    std::shared_ptr<InodeWrapper> inodeWrapper;
    CURVEFS_ERROR ret = inodeManager_->GetInode(ino, inodeWrapper);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager get inode fail, ret = " << ret
                   << ", inodeid = " << ino;
        return ret;
    }
    *linkStr = inodeWrapper->GetSymlinkStr();
    return CURVEFS_ERROR::OK;
}

CURVEFS_ERROR FuseClient::FuseOpRelease(fuse_req_t req, fuse_ino_t ino,
                                        struct fuse_file_info *fi) {
    LOG(INFO) << "FuseOpRelease, ino: " << ino;
    CURVEFS_ERROR ret = CURVEFS_ERROR::OK;
    std::shared_ptr<InodeWrapper> inodeWrapper;
    ret = inodeManager_->GetInode(ino, inodeWrapper);
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager get inode fail, ret = " << ret
                   << ", ino: " << ino;
        return ret;
    }

    ::curve::common::UniqueLock lgGuard = inodeWrapper->GetUniqueLock();

    ret = inodeWrapper->Release();
    if (ret != CURVEFS_ERROR::OK) {
        LOG(ERROR) << "inodeManager release inode fail, ret = " << ret
                   << ", ino: " << ino;
        return ret;
    }

    LOG(INFO) << "FuseOpRelease, ino: " << ino << " success";
    return ret;
}

void FuseClient::FlushInode() { inodeManager_->FlushInodeOnce(); }

void FuseClient::FlushInodeAll() { inodeManager_->FlushAll(); }

void FuseClient::FlushAll() {
    FlushData();
    FlushInodeAll();
}

}  // namespace client
}  // namespace curvefs
