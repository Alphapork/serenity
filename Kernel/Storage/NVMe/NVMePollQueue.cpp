/*
 * Copyright (c) 2022, Pankaj R <pankydev8@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Kernel/Arch/Delay.h>
#include <Kernel/Devices/BlockDevice.h>
#include <Kernel/Storage/NVMe/NVMeDefinitions.h>
#include <Kernel/Storage/NVMe/NVMePollQueue.h>

namespace Kernel {
UNMAP_AFTER_INIT NVMePollQueue::NVMePollQueue(NonnullOwnPtr<Memory::Region> rw_dma_region, Memory::PhysicalPage const& rw_dma_page, u16 qid, u32 q_depth, OwnPtr<Memory::Region> cq_dma_region, Vector<NonnullRefPtr<Memory::PhysicalPage>> cq_dma_page, OwnPtr<Memory::Region> sq_dma_region, Vector<NonnullRefPtr<Memory::PhysicalPage>> sq_dma_page, Memory::TypedMapping<DoorbellRegister volatile> db_regs)
    : NVMeQueue(move(rw_dma_region), rw_dma_page, qid, q_depth, move(cq_dma_region), cq_dma_page, move(sq_dma_region), sq_dma_page, move(db_regs))
{
}

void NVMePollQueue::submit_sqe(NVMeSubmission& sub)
{
    NVMeQueue::submit_sqe(sub);
    SpinlockLocker lock_cq(m_cq_lock);
    while (!process_cq()) {
        microseconds_delay(1);
    }
}

void NVMePollQueue::complete_current_request(u16 cmdid, u16 status)
{
    SpinlockLocker lock(m_request_lock);
    auto& request_pdu = m_requests.get(cmdid).release_value();
    auto current_request = request_pdu.request;
    AsyncDeviceRequest::RequestResult req_result = AsyncDeviceRequest::Success;

    ScopeGuard guard = [req_result, status, &request_pdu] {
        if (request_pdu.request)
            request_pdu.request->complete(req_result);
        if (request_pdu.end_io_handler)
            request_pdu.end_io_handler(status);
        request_pdu.used = false;
    };

    // There can be submission without any request associated with it such as with
    // admin queue commands during init. If there is no request, we are done
    if (!current_request)
        return;

    if (status) {
        req_result = AsyncBlockDeviceRequest::Failure;
        return;
    }

    if (current_request->request_type() == AsyncBlockDeviceRequest::RequestType::Read) {
        if (auto result = current_request->write_to_buffer(current_request->buffer(), m_rw_dma_region->vaddr().as_ptr(), current_request->buffer_size()); result.is_error()) {
            req_result = AsyncBlockDeviceRequest::MemoryFault;
            return;
        }
    }

    return;
}
}
