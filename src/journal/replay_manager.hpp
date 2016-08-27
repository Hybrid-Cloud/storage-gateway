/*
 * replay_manager.hpp
 *
 *  Created on: 2016Äê7ÔÂ28ÈÕ
 *      Author: smile-luobin
 */

#ifndef JOURNAL_REPLAY_MANAGER_HPP_
#define JOURNAL_REPLAY_MANAGER_HPP_

#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "journal_replayer.hpp"
#include "./cache/cache_proxy.h"

namespace Journal {

class ReplayManager {
public:
    ReplayManager(int replay_interval,
            int update_interval,
            const std::string& rpc_addr,
            std::vector<std::string> vols,
            std::vector<std::string> devices,
            std::shared_ptr<IDGenerator> id_maker_ptr);
    bool add_vol_replayer(const std::string& vol_id, const std::string& device);
    bool remove_vol_replayer(const std::string& vol_id);
    std::shared_ptr<CacheProxy> get_cache_proxy_ptr(const std::string& vol_id);
private:
    void start_replay();
    void recover_cache();

    int replay_interval;  //replay interval
    int update_interval;
    std::string rpc_addr;
    std::mutex map_mutex;

    std::shared_ptr<IDGenerator> id_maker_ptr;
    std::map<std::string, std::shared_ptr<CacheProxy>> vol_proxy_map;
    std::map<std::string, std::string> vol_device_map;
    std::map<std::string, std::unique_ptr<JournalReplayer>> vol_replayer_map;
};

}

#endif /* JOURNAL_REPLAY_MANAGER_HPP_ */
