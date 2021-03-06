/**********************************************
* Copyright (c) 2016 Huawei Technologies Co., Ltd. All rights reserved.
* 
* File name:    task_handler.cc
* Author: 
* Date:         2016/10/21
* Version:      1.0
* Description:
* 
************************************************/
#include<fstream>
#include "task_handler.h"
#include "log/log.h"
#include "../sg_util.h"
#include <stdlib.h>// posix_memalign
#include <chrono>
using std::string;
using grpc::Status;
using google::protobuf::int32;
using huawei::proto::transfer::MessageType;

#define TASK_HANDLER_THREAD_COUNT (8)

void TaskHandler::init(
        std::shared_ptr<BlockingQueue<std::shared_ptr<TransferTask>>> in,
        std::shared_ptr<BlockingQueue<std::shared_ptr<MarkerContext>>> out){
    tp_.reset(new sg_threads::ThreadPool(TASK_HANDLER_THREAD_COUNT,TASK_HANDLER_THREAD_COUNT));
    running_ = true;
    seq_id_ = 0L;
    in_task_que_ = in;
    out_que_ = out;
    for(int i=0; i<TASK_HANDLER_THREAD_COUNT; i++){
        tp_->submit(std::bind(&TaskHandler::work,this));
    }
}

TaskHandler::TaskHandler(){
}
TaskHandler::~TaskHandler(){
    running_ = false;
}

int TaskHandler::add_marker_context(ReplicatorContext* rep_ctx,
            const JournalMarker& marker){
    std::shared_ptr<MarkerContext> marker_ctx(new MarkerContext(rep_ctx,marker));
    if(out_que_->push(marker_ctx))
        return 0;
    else
        return -1;
}
void TaskHandler::wait_for_grpc_stream_ready(ClientContext* rpc_ctx,
        grpc_stream_ptr& stream){
    while(running_){
        stream = NetSender::instance().create_stream(rpc_ctx);
        if(nullptr == stream){
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        LOG_INFO << "create grpc stream success!";
        break;
    }
}

void TaskHandler::work(){
    // init stream
    ClientContext* rpc_ctx = new ClientContext;
    grpc_stream_ptr stream;
    wait_for_grpc_stream_ready(rpc_ctx,stream);

    // do TransferTask
    while(running_){
        std::shared_ptr<TransferTask> task = in_task_que_->pop();
        SG_ASSERT(task != nullptr);
        if(0 == do_transfer(task,stream)){
            task->set_status(T_DONE);
            // note: if failed, do not run callback function, 
            // or task window goes wrong
            std::shared_ptr<RepContext> rep_ctx =
                std::dynamic_pointer_cast<RepContext>(task->get_context());
            LOG_DEBUG << "task id:" << task->get_id()
                << ",vol:" << rep_ctx->get_vol_id()
                << ",journal:" << rep_ctx->get_j_counter();
            rep_ctx->get_callback()(task);
        }
        else{
            ClientState s = NetSender::instance().get_state(false);
            LOG_INFO << "rpc channel state:"
                << NetSender::instance().get_printful_state(s);
            if(CLIENT_READY != s){
                stream->WritesDone();
                stream->Finish();
                if(rpc_ctx){
                    delete rpc_ctx;
                    rpc_ctx = nullptr;
                }
                LOG_WARN << " re-create rpc stream...";
                rpc_ctx = new ClientContext;
                wait_for_grpc_stream_ready(rpc_ctx,stream);
            }
        }
    }

    // recycel rpc resource
    stream->WritesDone();
    Status status = stream->Finish();// may block if not all data in the stream were read
    if (!status.ok()) {
        LOG_ERROR << "replicate client close stream failed!";
    }
    if(rpc_ctx){
        delete rpc_ctx;
        rpc_ctx = nullptr;
    }
}

int send_replicate_cmd(TransferRequest* req,
        grpc_stream_ptr& stream){
    if(stream->Write(*req)){
        TransferResponse res;
        if(stream->Read(&res)){ // blocked
            SG_ASSERT(res.id() == req->id());
            if(!res.status()){
                return 0;
            }
            LOG_ERROR << "destination handle replicate cmd failed!";
        }
        else{
            LOG_ERROR << "replicate grpc read failed!";
        }
    }
    else{
        LOG_ERROR << "replicate grpc write failed!";
        return -1;
    }
    return -1;
}


int TaskHandler::do_transfer(std::shared_ptr<TransferTask> task,
                grpc_stream_ptr& stream){
    LOG_DEBUG << "start transfer task, id=" << task->get_id();

    bool error_flag = false;
    while(task->has_next_package()){
        TransferRequest* req = task->get_next_package();
        if(req == nullptr){
            LOG_ERROR << "get next package failed:" << task->get_id();
            error_flag = true;
            break;
        }
        switch(req->type()){
            case MessageType::REPLICATE_DATA:
                if(!stream->Write(*req)){
                    LOG_ERROR << "send replicate data failed!, task id:"
                        << task->get_id();
                    error_flag = true;
                }
                break;
            case MessageType::REPLICATE_START:
                if(send_replicate_cmd(req,stream) != 0){
                    LOG_ERROR << "handle replicate start req failed!, task id:"
                        << task->get_id();
                    task->set_status(T_ERROR);
                }
                break;
            case MessageType::REPLICATE_END:
                if(send_replicate_cmd(req,stream) != 0){
                    LOG_ERROR << "handle replicate end req failed!, task id:"
                        << task->get_id();
                    error_flag = true;
                }
                break;


            default:
                LOG_WARN << "unknown transfer message, type=" << req->type()
                    << ", id=" << req->id();
                break;
        }
        // recycle req
        delete req;
        req = nullptr;
        if(error_flag){
            task->set_status(T_ERROR);
            break;
        }
    }
    return error_flag? -1:0;
}

