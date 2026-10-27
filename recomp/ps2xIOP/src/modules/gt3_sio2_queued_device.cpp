#include "ps2x/iop/gt3/sio2_queued_device.h"
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>

namespace ps2x::iop::gt3 {
struct Sio2QueuedDevice::State {
    struct Job {
        Sio2Transfer request;
        Sio2Completion completion;
        std::exception_ptr error;
        bool done=false,success=false;
    };
    std::shared_ptr<Sio2Device> device;
    std::mutex mutex;
    std::condition_variable changed;
    std::shared_ptr<Job> job;
    bool stopped=false;
    std::thread worker;
    std::thread::id workerId;
    explicit State(std::shared_ptr<Sio2Device> source):device(std::move(source)) {
        worker=std::thread([this] {
            std::unique_lock lock(mutex);
            for (;;) {
                changed.wait(lock,[this] { return stopped || (job && !job->done); });
                if (stopped) break;
                auto current=job;
                lock.unlock();
                try { current->success=device && device->exchange(current->request,current->completion); }
                catch (...) { current->error=std::current_exception(); }
                lock.lock();
                if (stopped) { current->success=false; current->error={}; }
                current->done=true;
                changed.notify_all();
            }
        });
        workerId=worker.get_id();
    }
};
Sio2QueuedDevice::Sio2QueuedDevice(std::shared_ptr<Sio2Device> device)
    :state_(std::make_unique<State>(std::move(device))) {}
Sio2QueuedDevice::~Sio2QueuedDevice() { stop(); }
void Sio2QueuedDevice::stop() {
    auto& state=*state_;
    {
        std::lock_guard lock(state.mutex);
        state.stopped=true;
        state.changed.notify_all();
    }
    if (state.worker.joinable()) state.worker.join();
}
bool Sio2QueuedDevice::exchange(const Sio2Transfer& request,Sio2Completion& completion) {
    auto& state=*state_;
    std::unique_lock lock(state.mutex);
    if (state.stopped || state.job || std::this_thread::get_id()==state.workerId ||
        request.input.size()>Sio2MaxFifoBytes || request.dmaInput.size()>Sio2MaxDmaBytes ||
        request.outputSize>Sio2MaxFifoBytes || request.dmaOutputSize>Sio2MaxDmaBytes) return false;
    auto job=std::make_shared<State::Job>();
    job->request=request;
    state.job=job;
    state.changed.notify_all();
    state.changed.wait(lock,[&] { return job->done || state.stopped; });
    // A running callback owns the job until the worker completes it, even when
    // shutdown wakes the caller first. No callback references caller storage.
    if (job->done) state.job.reset();
    if (state.stopped) return false;
    if (job->error) std::rethrow_exception(job->error);
    if (!job->success) return false;
    completion=std::move(job->completion);
    return true;
}
}
