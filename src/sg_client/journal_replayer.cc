/*
 * journal_replayer.cpp
 *
 *  Created on: 2016Äê7ÔÂ14ÈÕ
 *      Author: smile-luobin
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <cstdio>
#include <errno.h>
#include <boost/bind.hpp>
#include "journal_replayer.h"
#include "volume.h"

#include "../log/log.h"
#include "../rpc/message.pb.h"

using google::protobuf::Message;
using huawei::proto::WriteMessage;
using huawei::proto::SnapshotMessage;
using huawei::proto::DiskPos;
using huawei::proto::SnapScene;
using huawei::proto::SnapType;
using huawei::proto::RepStatus;
using huawei::proto::RepRole;

using namespace std;

namespace Journal
{

JournalReplayer::JournalReplayer(VolumeAttr& vol_attr)
                :vol_attr_(vol_attr)
{
}

bool JournalReplayer::init(const std::string& rpc_addr,
                           std::shared_ptr<IDGenerator> id_maker_ptr,
                           std::shared_ptr<CacheProxy> cache_proxy_ptr,
                           std::shared_ptr<SnapshotProxy> snapshot_proxy_ptr,
                           std::shared_ptr<ReplicateProxy> rep_proxy_ptr)
{
    id_maker_ptr_       = id_maker_ptr;
    cache_proxy_ptr_    = cache_proxy_ptr;
    snapshot_proxy_ptr_ = snapshot_proxy_ptr;
    rep_proxy_ptr_      = rep_proxy_ptr;
    backup_decorator_ptr_.reset(new BackupDecorator(vol_attr_.vol_name(), snapshot_proxy_ptr));

    rpc_client_ptr_.reset(new ReplayerClient(grpc::CreateChannel(rpc_addr,
                            grpc::InsecureChannelCredentials())));

    cache_recover_ptr_.reset(new CacheRecovery(vol_attr_.vol_name(), rpc_client_ptr_, 
                                id_maker_ptr_, cache_proxy_ptr_));
    cache_recover_ptr_->start();
    //block until recover finish
    cache_recover_ptr_->stop(); 
    cache_recover_ptr_.reset();

    update_ = false;
    vol_fd_ = ::open(vol_attr_.blk_device().c_str(), O_WRONLY | O_DIRECT | O_SYNC);
    if (vol_fd_ < 0){
        LOG_ERROR << "open block device:" << vol_attr_.blk_device() << "failed";
        return false;
    }

    //start replay volume
    replay_thread_ptr_.reset(
            new boost::thread(
                    boost::bind(&JournalReplayer::replay_volume_loop, this)));
    //start update marker
    update_thread_ptr_.reset(
            new boost::thread(
                    boost::bind(&JournalReplayer::update_marker_loop, this)));

    return true;
}

bool JournalReplayer::deinit()
{
    /*todo: here force exit, not rational*/
    replay_thread_ptr_->interrupt();
    update_thread_ptr_->interrupt();
    replay_thread_ptr_->join();
    update_thread_ptr_->join();
    if(-1 != vol_fd_){
        ::close(vol_fd_);
    }
    return true;
}

void JournalReplayer::update_marker_loop()
{
    //todo: read config ini
    int_least64_t update_interval = 60;
    while (true){
        boost::this_thread::sleep_for(boost::chrono::seconds(update_interval));
        if (update_){
            rpc_client_ptr_->UpdateConsumerMarker(journal_marker_, vol_attr_.vol_name());
            LOG_INFO << "update marker succeed";
            update_ = false;
        }
    }
}

bool JournalReplayer::replay_each_journal(const string& journal, 
                                          const off_t& start_pos,
                                          const off_t& end_pos)
{
    bool retval = true;
    int  fd = -1;

    do {
        fd = ::open(journal.c_str(), O_RDONLY);
        if(-1 == fd){
            LOG_ERROR << "open " << journal.c_str() << "failed errno:" << errno;
            retval = false;
            break;
        }
        struct stat buf = {0};
        int ret = fstat(fd, &buf);
        if(-1 == ret){
            LOG_ERROR << "stat " << journal.c_str() << "failed errno:" << errno;
            retval = false; 
            break;
        }

        size_t size = buf.st_size;
        if(size == 0){
            retval = false;
            break; 
        }

        off_t start = start_pos;
        off_t end   = (end_pos < size) ? end_pos : size;
        LOG_INFO << "open file:" << journal << " start:" << start 
                 << " end:" << end << " size:" << size;

        while(start < end){
            string journal_file = journal;
            off_t  journal_off  = start;
            shared_ptr<JournalEntry> journal_entry = make_shared<JournalEntry>();
            start = journal_entry->parse(fd, start); 
            retval = process_journal_entry(journal_entry);
        }

    } while(0);
    
    if(-1 != fd){
        ::close(fd); 
    }
    return retval;
}

void JournalReplayer::replica_replay()
{
    /*in memory consumer mark empty*/
    if(journal_marker_.cur_journal().empty()){
        bool ret = rpc_client_ptr_->GetJournalMarker(vol_attr_.vol_name(), journal_marker_); 
        if(!ret || journal_marker_.cur_journal().empty()){
            LOG_ERROR << "get journal replay consumer marker failed";
            usleep(200);
            return;
        }
    }

    /*get replayer journal file list */
    constexpr int limit = 10;
    list<JournalElement> journal_list; 
    bool ret = rpc_client_ptr_->GetJournalList(vol_attr_.vol_name(), journal_marker_, 
                                          limit, journal_list);
    if(!ret || journal_list.empty()){
        LOG_ERROR << "get journal list failed";
        usleep(200);
        return;
    }

    /*replay each journal file*/
    for(auto it : journal_list){
        string journal  = "/mnt/cephfs" + it.journal();
        off_t start_pos = it.start_offset();
        off_t end_pos   = it.end_offset();
        ret= replay_each_journal(journal, start_pos, end_pos);    
        /*replay ok, update in memory consumer marker*/
        if(ret){
            update_consumer_marker(journal, end_pos);
        }
    }
}

void JournalReplayer::normal_replay()
{
    shared_ptr<CEntry> entry = cache_proxy_ptr_->pop();
    if(entry == nullptr){
        usleep(200);
        return;
    }

    if (entry->get_cache_type() == CEntry::IN_MEM){
        //replay from memory
        LOG_INFO << "replay from memory";
        bool succeed = process_memory(entry->get_journal_entry());
        if (succeed){
            update_consumer_marker(entry->get_journal_file(), 
                    entry->get_journal_off());
            cache_proxy_ptr_->reclaim(entry);
        }
    } else {
        //replay from journal file
        LOG_INFO << "replay from journal file";
        bool succeed = process_file(entry);
        if (succeed){
            update_consumer_marker(entry->get_journal_file(), 
                    entry->get_journal_off());
            cache_proxy_ptr_->reclaim(entry);
        }
    }
}

void JournalReplayer::replay_volume_loop()
{
    while (true)
    {
        int cur_mode = vol_attr_.current_replay_mode();
        if(cur_mode == VolumeAttr::NORMAL_REPLAY_MODE){
            normal_replay();
        } else {
            replica_replay();
        }
    }
}

bool JournalReplayer::handle_io_cmd(shared_ptr<JournalEntry> entry)
{
    shared_ptr<Message> message = entry->get_message();
    shared_ptr<WriteMessage> write = dynamic_pointer_cast<WriteMessage>(message);

    int pos_num = write->pos_size();
    char* data = (char*)write->data().c_str();

    /*entry contain merged io*/
    for(int i = 0; i < pos_num; i++){
        DiskPos* pos = write->mutable_pos(i);
        off_t  off = pos->offset();
        size_t len = pos->length();

        /*todo: direct io need memory align*/
        void *align_buf = nullptr;
        int ret = posix_memalign((void**) &align_buf, 512, len);
        assert(ret == 0 && align_buf != nullptr);
        memcpy(align_buf, data, len);
        if(!snapshot_proxy_ptr_->check_exist_snapshot()){
            ret = pwrite(vol_fd_, align_buf, len, off);
        } else {
            ret = snapshot_proxy_ptr_->do_cow(off, len, (char*)align_buf, false);
        }
        free(align_buf);

        /*next io data offset*/
        data += len;
    }

    return true;
}

bool JournalReplayer::handle_ctrl_cmd(shared_ptr<JournalEntry> entry)
{
    /*handle snapshot*/
    int type = entry->get_type();
    if(type == SNAPSHOT_CREATE || type == SNAPSHOT_DELETE || type == SNAPSHOT_ROLLBACK){
        shared_ptr<Message> message = entry->get_message();
        shared_ptr<SnapshotMessage> snap_message = dynamic_pointer_cast
                                                    <SnapshotMessage>(message);
        SnapReqHead shead;
        shead.set_replication_uuid(snap_message->replication_uuid());
        shead.set_checkpoint_uuid(snap_message->checkpoint_uuid());
        shead.set_scene((SnapScene)snap_message->snap_scene());
        shead.set_snap_type((SnapType)snap_message->snap_type());
        string snap_name = snap_message->snap_name();
        SnapScene scene  = (SnapScene)snap_message->snap_scene();
        
        if(scene == SnapScene::FOR_NORMAL){
            switch(type){
                case SNAPSHOT_CREATE:
                    LOG_INFO << "journal_replayer create snapshot:" << snap_name;
                    snapshot_proxy_ptr_->create_transaction(shead, snap_name);
                    break;
                case SNAPSHOT_DELETE:
                    LOG_INFO << "journal_replayer delete snapshot:" << snap_name;
                    snapshot_proxy_ptr_->delete_transaction(shead, snap_name);
                    break;
                case SNAPSHOT_ROLLBACK:
                    LOG_INFO << "journal_replayer rollback snapshot:" << snap_name;
                    snapshot_proxy_ptr_->rollback_transaction(shead, snap_name);
                    break;
                default:
                    break;
            }
        } else if(scene == SnapScene::FOR_BACKUP){
            switch(type){
                case SNAPSHOT_CREATE:
                    LOG_INFO << "journal_replayer create snapshot:" << snap_name;
                    backup_decorator_ptr_->create_transaction(shead, snap_name);
                    break;
                default:
                    break;
            }
        } else if(scene == SnapScene::FOR_REPLICATION){
            switch(type){
                case SNAPSHOT_CREATE:
                    LOG_INFO << "journal_replayer create snapshot:" << snap_name;
                    rep_proxy_ptr_->create_transaction(shead, snap_name);
                    break;
                default:
                    break;
            }
        } else {

        }
        return true;
    } else {
    
    } 

    return false;
}

bool JournalReplayer::process_journal_entry(shared_ptr<JournalEntry> entry)
{
    bool retval = false;
    journal_event_type_t type = entry->get_type();
    if(IO_WRITE == type){
        retval = handle_io_cmd(entry);
    } else {
        retval = handle_ctrl_cmd(entry);
    }
    return retval;
}

bool JournalReplayer::process_memory(std::shared_ptr<JournalEntry> entry)
{
   return process_journal_entry(entry);
}

bool JournalReplayer::process_file(shared_ptr<CEntry> entry)
{
    string file_name = entry->get_journal_file();
    off_t  off = entry->get_journal_off();

    /*todo avoid open frequently*/
    int src_fd = ::open(file_name.c_str(), O_RDONLY);
    if (src_fd == -1){
        LOG_ERROR << "open journal file failed";
        return false;
    }
    
    shared_ptr<JournalEntry> jentry = make_shared<JournalEntry>();
    jentry->parse(src_fd, off);
       
    if(src_fd != -1){
        ::close(src_fd); 
    }
    
    return process_journal_entry(jentry);
}

void JournalReplayer::update_consumer_marker(const string& journal,
                                             const off_t&  off)
{
    std::unique_lock<std::mutex> ul(journal_marker_mutex_);
    journal_marker_.set_cur_journal(journal.c_str());
    journal_marker_.set_pos(off);
    LOG_INFO << "consumer marker file:" << journal << " pos:" << off;
    update_ = true;
}

}
