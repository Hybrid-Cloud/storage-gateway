#include "common/block_dev.h"
#include "log/log.h"
#include "control_snapshot.h"
using huawei::proto::StatusCode;

SnapshotControlImpl::SnapshotControlImpl(map<string, shared_ptr<Volume>>& volumes)
        :m_volumes(volumes)
{
    m_pending_queue = new BlockingQueue<struct BgJob*>(10);
    m_complete_queue = new deque<struct BgJob*>();
    m_run = true;
    m_work_thread = new thread(&SnapshotControlImpl::bg_work, this);
    m_reclaim_thread = new thread(&SnapshotControlImpl::bg_reclaim, this);
}

SnapshotControlImpl::~SnapshotControlImpl()
{
    m_pending_queue->stop();
    m_reclaim_thread->join();
    m_work_thread->join();
    delete m_reclaim_thread;
    delete m_work_thread;
    delete m_pending_queue;
    delete m_complete_queue;
    m_volumes.clear();
}

shared_ptr<SnapshotProxy> SnapshotControlImpl::get_vol_snap_proxy(const string& vol_name)
{
    auto it = m_volumes.find(vol_name);
    if(it != m_volumes.end()){
        return it->second->get_snapshot_proxy();
    }
    LOG_ERROR << "get_vol_snap_proxy vid:" << vol_name << "failed";
    return nullptr; 
}

Status SnapshotControlImpl::CreateSnapshot(ServerContext* context, 
                                           const CreateSnapshotReq* req, 
                                           CreateSnapshotAck* ack) 
{
    /*find volume*/
    string vname = req->vol_name();
    LOG_INFO << "RPC CreateSnapshot vname:" << vname;
    shared_ptr<SnapshotProxy> vol_snap_proxy = get_vol_snap_proxy(vname);
    assert(vol_snap_proxy != nullptr);
    /*dispatch to volume*/
    StatusCode ret = vol_snap_proxy->create_snapshot(req, ack);
    if(ret != StatusCode::sOk){
        LOG_ERROR << "RPC CreateSnapshot vname:" << vname 
                  << " failed" << " err:" << ret;
        return Status::CANCELLED;
    }
    LOG_INFO << "RPC CreateSnapshot vname:" << vname << " ok";
    return Status::OK;
}

Status SnapshotControlImpl::ListSnapshot(ServerContext* context, 
                                         const ListSnapshotReq* req, 
                                         ListSnapshotAck* ack)
{    
    string vname = req->vol_name();
    LOG_INFO << "RPC ListSnapshot vname:" << vname;
    shared_ptr<SnapshotProxy> vol_snap_proxy = get_vol_snap_proxy(vname);
    assert(vol_snap_proxy != nullptr);
    
    /*dispatch to volume*/
    StatusCode ret = vol_snap_proxy->list_snapshot(req, ack);
    if(ret != StatusCode::sOk){
        LOG_ERROR << "RPC ListSnapshot vname:" << vname 
                  << " failed" << " err:" << ret;
        return Status::CANCELLED;
    }

    LOG_INFO << "RPC ListSnapshot vname:" << vname << " ok";
    return Status::OK;
}

Status SnapshotControlImpl::QuerySnapshot(ServerContext* context, 
                                          const QuerySnapshotReq* req, 
                                          QuerySnapshotAck* ack)
{    
    string vname = req->vol_name();
    LOG_INFO << "RPC QuerySnapshot vname:" << vname;
    shared_ptr<SnapshotProxy> vol_snap_proxy = get_vol_snap_proxy(vname);
    assert(vol_snap_proxy != nullptr);
    
    /*dispatch to volume*/
    StatusCode ret = vol_snap_proxy->query_snapshot(req, ack);
    if(ret != StatusCode::sOk){
        LOG_ERROR << "RPC QuerySnapshot vname:" << vname 
                  << " failed" << " err:" << ret;
        return Status::CANCELLED;
    }

    LOG_INFO << "RPC QuerySnapshot vname:" << vname << " ok";
    return Status::OK;
}

Status SnapshotControlImpl::DeleteSnapshot(ServerContext* context, 
                                           const DeleteSnapshotReq* req, 
                                           DeleteSnapshotAck* ack)
{    
    string vname = req->vol_name();
    LOG_INFO << "RPC DeleteSnapshot" << " vname:" << vname;

    shared_ptr<SnapshotProxy> vol_snap_proxy = get_vol_snap_proxy(vname);
    assert(vol_snap_proxy != nullptr);
    
    /*dispatch to volume*/
    StatusCode ret = vol_snap_proxy->delete_snapshot(req, ack);
    if(ret != StatusCode::sOk){
        LOG_ERROR << "RPC DeleteSnapshot vname:" << vname 
                  << " failed" << " err:" << ret;
        return Status::CANCELLED;
    }

    LOG_INFO << "RPC DeleteSnapshot" << " vname:" << vname << " ok";
    return Status::OK;
}

Status SnapshotControlImpl::RollbackSnapshot(ServerContext* context, 
                                             const RollbackSnapshotReq* req, 
                                             RollbackSnapshotAck* ack) 
{
    string vname = req->vol_name();
    LOG_INFO << "RPC RollbackSnapshot" << " vname:" << vname;

    shared_ptr<SnapshotProxy> vol_snap_proxy = get_vol_snap_proxy(vname);
    assert(vol_snap_proxy != nullptr);
    
    /*dispatch to volume*/
    StatusCode ret = vol_snap_proxy->rollback_snapshot(req, ack);
    if(ret != StatusCode::sOk){
        LOG_ERROR << "RPC RollbackSnapshot vname:" 
                 << vname << " failed" << " err:" << ret;
        return Status::CANCELLED;
    }

    LOG_INFO << "RPC RollbackSnapshot" << " vname:" << vname << " ok";
    return Status::OK;
}

 Status SnapshotControlImpl::DiffSnapshot(ServerContext* context, 
                                          const DiffSnapshotReq* req, 
                                          DiffSnapshotAck* ack)
{
    string vname = req->vol_name();
    LOG_INFO << "RPC DiffSnapshot" << " vname:" << vname;

    shared_ptr<SnapshotProxy> vol_snap_proxy = get_vol_snap_proxy(vname);
    assert(vol_snap_proxy != nullptr);
    
    /*dispatch to volume*/
    StatusCode ret = vol_snap_proxy->diff_snapshot(req, ack);
    if(ret != StatusCode::sOk){
        LOG_ERROR << "RPC DiffSnapshot vname:" << vname  
                  << " failed" << " err:" << ret;
        return Status::CANCELLED;
    }

    LOG_INFO << "RPC DiffSnapshot vname:" << vname << " ok"; 
    return Status::OK;
}

Status SnapshotControlImpl::ReadSnapshot(ServerContext* context, 
                                         const ReadSnapshotReq* req, 
                                         ReadSnapshotAck* ack)
{
    string vname = req->vol_name();
    LOG_INFO << "RPC ReadSnapshot" << " vname:" << vname;

    shared_ptr<SnapshotProxy> vol_snap_proxy = get_vol_snap_proxy(vname);
    assert(vol_snap_proxy != nullptr);
    
    /*dispatch to volume*/
    StatusCode ret = vol_snap_proxy->read_snapshot(req, ack);
    if(ret != StatusCode::sOk){
        LOG_ERROR << "RPC ReadSnapshot vname:" << vname 
                  << " failed" << " err:" << ret;
        return Status::CANCELLED;
    }

    LOG_INFO << "RPC ReadSnapshot vname:" << vname 
             << "data size:" << ack->data().size() 
             << "data_len:" << ack->data().length() << " ok"; 
    return Status::OK;
}

Status SnapshotControlImpl::CreateVolumeFromSnap(ServerContext* context, 
                                                 const CreateVolumeFromSnapReq* req, 
                                                 CreateVolumeFromSnapAck* ack)
{
    string new_volume = req->new_vol_name();
    string new_blk_device = req->new_blk_device();
    string vname = req->vol_name();
    string sname = req->snap_name();
    
    LOG_INFO << "RPC CreateVolumeFromSnap vname:" << vname;
    if(m_pending_queue->full()){
        LOG_INFO << "RPC CreateVolumeFromSnap vname:" << vname << "queue full failed";
        ack->mutable_header()->set_status(StatusCode::sSnapCreateVolumeBusy);
        return Status::CANCELLED;
    }

    struct BgJob* job = (struct BgJob*)malloc(sizeof(struct BgJob)); 
    job->new_volume = new_volume;
    job->new_blk_device = new_blk_device;
    job->vol_name = vname;
    job->snap_name = sname;
    job->status = BG_INIT;
    m_pending_queue->push(job);

    ack->mutable_header()->set_status(StatusCode::sOk);
    LOG_INFO << "RPC CreateVolumeFromSnap vname:" << vname  << endl;
    return Status::OK;
}

Status SnapshotControlImpl::QueryVolumeFromSnap(ServerContext* context, 
                                                const QueryVolumeFromSnapReq* req, 
                                                QueryVolumeFromSnapAck* ack)
{
    string new_volume = req->new_vol_name();
    LOG_INFO << "RPC QueryVolumeFromSnap vname:" << new_volume;
    for(int i = 0; i < m_pending_queue->size(); i++)
    {
        struct BgJob* job = (*m_pending_queue)[i];
        if(job->new_volume.compare(new_volume) == 0){
            ack->set_status(VolumeStatus::VOL_ENABLING);
            return Status::OK;
        }
    }

    for(int i = 0; i < m_complete_queue->size(); i++)
    {
        struct BgJob* job = (*m_complete_queue)[i];
        if(job->new_volume.compare(new_volume) == 0){
            ack->set_status(VolumeStatus::VOL_AVAILABLE);
            return Status::OK;
        }
    }

    LOG_INFO << "RPC QueryVolumeFromSnap vname:" << new_volume  << endl;
    return Status::OK;
}

void SnapshotControlImpl::bg_work()
{
    while(m_run)
    {
        struct BgJob* job = m_pending_queue->pop();
        if(job == nullptr)
            return;
        job->status = BG_DOING;
        gettimeofday(&(job->start_ts), NULL);
        
        BlockDevice bdev(job->new_blk_device);
        size_t bdev_size = bdev.dev_size();
        off_t  bdev_off  = 0;
        size_t bdev_slice = COW_BLOCK_SIZE;

        shared_ptr<SnapshotProxy> vol_snap_proxy = get_vol_snap_proxy(job->vol_name);
        assert(vol_snap_proxy != nullptr);
        ReadSnapshotReq req;
        req.set_vol_name(job->vol_name);
        req.set_snap_name(job->snap_name);
        ReadSnapshotAck ack;
        while(bdev_off < bdev_size)
        {
            bdev_slice = ((bdev_size-bdev_off) > COW_BLOCK_SIZE) ? COW_BLOCK_SIZE : (bdev_size-bdev_off);
            req.set_off(bdev_off);
            req.set_len(bdev_slice);
            StatusCode ret = vol_snap_proxy->read_snapshot(&req, &ack);
            assert(ret == StatusCode::sOk);

            bdev.sync_write(ack.data().c_str(), ack.data().length(), bdev_off);
            bdev_off += bdev_slice;
        }
        job->status = BG_DONE;
        gettimeofday(&(job->complete_ts), NULL);
        gettimeofday(&(job->expire_ts), NULL);
        job->expire_ts.tv_sec += (60*60*60); //60 hours

        m_complete_queue->push_back(job);
   }
}

void SnapshotControlImpl::bg_reclaim()
{
    while(m_run)
    {
        struct BgJob* job = m_complete_queue->front();
        struct timeval current;
        gettimeofday(&current, NULL);
        if(current.tv_sec >= job->expire_ts.tv_sec){
            m_complete_queue->pop_front();
            free(job); 
        }
        sleep(60);
    }
}
