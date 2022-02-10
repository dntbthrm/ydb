#include "boot_queue.h"
#include "leader_tablet_info.h" 

namespace NKikimr {
namespace NHive {

void TBootQueue::AddToBootQueue(TBootQueueRecord record) {
    BootQueue.push(record);
}

TBootQueue::TBootQueueRecord TBootQueue::PopFromBootQueue() {
    TBootQueueRecord record = BootQueue.top();
    BootQueue.pop();
    return record;
}

void TBootQueue::AddToWaitQueue(TBootQueueRecord record) {
    WaitQueue.emplace_back(record);
}

void TBootQueue::MoveFromWaitQueueToBootQueue() {
    for (TBootQueueRecord record : WaitQueue) {
        AddToBootQueue(record);
    }
    WaitQueue.clear();
}

}
}
