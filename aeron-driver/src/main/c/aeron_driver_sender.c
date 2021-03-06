/*
 * Copyright 2014 - 2017 Real Logic Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if defined(__linux__)
#define _GNU_SOURCE
#endif

#include <sys/socket.h>
#include <stdio.h>

#if !defined(HAVE_RECVMMSG)
struct mmsghdr
{
    struct msghdr msg_hdr;
    unsigned int msg_len;
};
#endif

#include "util/aeron_arrayutil.h"
#include "media/aeron_send_channel_endpoint.h"
#include "aeron_driver_sender.h"
#include "aeron_driver_conductor_proxy.h"

int aeron_driver_sender_init(
    aeron_driver_sender_t *sender,
    aeron_driver_context_t *context,
    aeron_system_counters_t *system_counters,
    aeron_distinct_error_log_t *error_log)
{
    if (aeron_udp_transport_poller_init(&sender->poller) < 0)
    {
        return -1;
    }

    for (size_t i = 0; i < AERON_DRIVER_SENDER_NUM_RECV_BUFFERS; i++)
    {
        size_t offset = 0;
        if (aeron_alloc_aligned(
            (void **)&sender->recv_buffers.buffers[i],
            &offset,
            context->mtu_length,
            AERON_CACHE_LINE_LENGTH * 2) < 0)
        {
            return -1;
        }

        sender->recv_buffers.iov[i].iov_base = sender->recv_buffers.buffers[i] + offset;
        sender->recv_buffers.iov[i].iov_len = context->mtu_length;
    }

    sender->context = context;
    sender->error_log = error_log;
    sender->sender_proxy.sender = sender;
    sender->sender_proxy.command_queue = &context->sender_command_queue;
    sender->sender_proxy.fail_counter =
        aeron_system_counter_addr(system_counters, AERON_SYSTEM_COUNTER_SENDER_PROXY_FAILS);
    sender->sender_proxy.threading_mode = context->threading_mode;

    sender->network_publicaitons.array = NULL;
    sender->network_publicaitons.length = 0;
    sender->network_publicaitons.capacity = 0;

    sender->round_robin_index = 0;
    sender->duty_cycle_counter = 0;
    sender->duty_cycle_ratio = context->send_to_sm_poll_ratio;
    sender->status_message_read_timeout_ns = context->status_message_timeout_ns / 2;
    sender->control_poll_timeout_ns = 0;
    sender->total_bytes_sent_counter =
        aeron_system_counter_addr(system_counters, AERON_SYSTEM_COUNTER_BYTES_SENT);
    sender->errors_counter =
        aeron_system_counter_addr(system_counters, AERON_SYSTEM_COUNTER_ERRORS);
    return 0;
}

#define AERON_DRIVER_SENDER_ERROR(sender, format, ...) \
do \
{ \
    char error_buffer[AERON_MAX_PATH]; \
    int err_code = aeron_errcode(); \
    snprintf(error_buffer, sizeof(error_buffer) - 1, format, __VA_ARGS__); \
    aeron_distinct_error_log_record(sender->error_log, err_code, aeron_errmsg(), error_buffer); \
    aeron_counter_increment(sender->errors_counter, 1); \
    aeron_set_err(0, "%s", "no error"); \
} \
while(0)

void aeron_driver_sender_on_command(void *clientd, volatile void *item)
{
    aeron_driver_sender_t *sender = (aeron_driver_sender_t *)clientd;
    aeron_command_base_t *cmd = (aeron_command_base_t *)item;

    cmd->func(clientd, cmd);

    /* recycle cmd by sending to conductor as on_cmd_free */
    aeron_driver_conductor_proxy_on_delete_cmd(sender->context->conductor_proxy, cmd);
}

int aeron_driver_sender_do_work(void *clientd)
{
    aeron_driver_sender_t *sender = (aeron_driver_sender_t *)clientd;
    int work_count = 0;

    work_count +=
        aeron_spsc_concurrent_array_queue_drain(
            sender->sender_proxy.command_queue, aeron_driver_sender_on_command, sender, 10);

    int64_t now_ns = sender->context->nano_clock();
    int bytes_sent = aeron_driver_sender_do_send(sender, now_ns);
    int poll_result;

    if (0 == bytes_sent ||
        ++sender->duty_cycle_counter == sender->duty_cycle_ratio ||
        now_ns > sender->control_poll_timeout_ns)
    {
        struct mmsghdr mmsghdr[AERON_DRIVER_SENDER_NUM_RECV_BUFFERS];

        for (size_t i = 0; i < AERON_DRIVER_SENDER_NUM_RECV_BUFFERS; i++)
        {
            mmsghdr[i].msg_hdr.msg_name = &sender->recv_buffers.addrs[i];
            mmsghdr[i].msg_hdr.msg_namelen = sizeof(sender->recv_buffers.addrs[i]);
            mmsghdr[i].msg_hdr.msg_iov = &sender->recv_buffers.iov[i];
            mmsghdr[i].msg_hdr.msg_iovlen = 1;
            mmsghdr[i].msg_hdr.msg_flags = 0;
            mmsghdr[i].msg_len = 0;
        }

        poll_result = aeron_udp_transport_poller_poll(
            &sender->poller,
            mmsghdr,
            AERON_DRIVER_SENDER_NUM_RECV_BUFFERS,
            aeron_send_channel_endpoint_dispatch,
            sender);

        if (poll_result < 0)
        {
            AERON_DRIVER_SENDER_ERROR(sender, "sender poller_poll: %s", aeron_errmsg());
        }

        work_count += (poll_result < 0) ? 0 : poll_result;

        sender->duty_cycle_counter = 0;
        sender->control_poll_timeout_ns = now_ns + sender->status_message_read_timeout_ns;
    }

    return work_count + bytes_sent;
}

void aeron_driver_sender_on_close(void *clientd)
{
    aeron_driver_sender_t *sender = (aeron_driver_sender_t *)clientd;

    for (size_t i = 0; i < AERON_DRIVER_SENDER_NUM_RECV_BUFFERS; i++)
    {
        aeron_free(sender->recv_buffers.buffers[i]);
    }

    aeron_udp_transport_poller_close(&sender->poller);
    aeron_free(sender->network_publicaitons.array);
}

void aeron_driver_sender_on_add_endpoint(void *clientd, void *command)
{
    aeron_driver_sender_t *sender = (aeron_driver_sender_t *)clientd;
    aeron_command_base_t *cmd = (aeron_command_base_t *)command;
    aeron_send_channel_endpoint_t *endpoint = (aeron_send_channel_endpoint_t *)cmd->item;

    if (aeron_udp_transport_poller_add(&sender->poller, &endpoint->transport) < 0)
    {
        AERON_DRIVER_SENDER_ERROR(sender, "sender on_add_endpoint: %s", aeron_errmsg());
    }
}

void aeron_driver_sender_on_remove_endpoint(void *clientd, void *command)
{
    aeron_driver_sender_t *sender = (aeron_driver_sender_t *)clientd;
    aeron_command_base_t *cmd = (aeron_command_base_t *)command;
    aeron_send_channel_endpoint_t *endpoint = (aeron_send_channel_endpoint_t *)cmd->item;

    if (aeron_udp_transport_poller_remove(&sender->poller, &endpoint->transport) < 0)
    {
        AERON_DRIVER_SENDER_ERROR(sender, "sender on_remove_endpoint: %s", aeron_errmsg());
    }
}

void aeron_driver_sender_on_add_publication(void *clientd, void *command)
{
    aeron_driver_sender_t *sender = (aeron_driver_sender_t *)clientd;
    aeron_command_base_t *cmd = (aeron_command_base_t *)command;
    aeron_network_publication_t *publication = (aeron_network_publication_t *)cmd->item;

    int ensure_capacity_result = 0;
    AERON_ARRAY_ENSURE_CAPACITY(
        ensure_capacity_result, sender->network_publicaitons, sizeof(aeron_driver_sender_network_publication_entry_t));

    if (ensure_capacity_result < 0)
    {
        AERON_DRIVER_SENDER_ERROR(sender, "sender on_add_publication: %s", aeron_errmsg());
        return;
    }

    sender->network_publicaitons.array[sender->network_publicaitons.length++].publication = publication;
    if (aeron_send_channel_endpoint_add_publication(publication->endpoint, publication) < 0)
    {
        AERON_DRIVER_SENDER_ERROR(sender, "sender on_add_publication add_publication: %s", aeron_errmsg());
    }
}

void aeron_driver_sender_on_remove_publication(void *clientd, void *command)
{
    aeron_driver_sender_t *sender = (aeron_driver_sender_t *)clientd;
    aeron_command_base_t *cmd = (aeron_command_base_t *)command;
    aeron_network_publication_t *publication = (aeron_network_publication_t *)cmd->item;

    for (size_t i = 0, size = sender->network_publicaitons.length, last_index = size - 1; i < size; i++)
    {
        if (publication == sender->network_publicaitons.array[i].publication)
        {
            aeron_array_fast_unordered_remove(
                (uint8_t *)sender->network_publicaitons.array,
                sizeof(aeron_driver_sender_network_publication_entry_t),
                i,
                last_index);
            sender->network_publicaitons.length--;
            break;
        }
    }

    if (aeron_send_channel_endpoint_remove_publication(publication->endpoint, publication) < 0)
    {
        AERON_DRIVER_SENDER_ERROR(sender, "sender on_remove_publication: %s", aeron_errmsg());
    }
}

int aeron_driver_sender_do_send(aeron_driver_sender_t *sender, int64_t now_ns)
{
    int bytes_sent = 0;
    aeron_driver_sender_network_publication_entry_t *publications = sender->network_publicaitons.array;
    size_t length = sender->network_publicaitons.length;
    size_t starting_index = sender->round_robin_index;

    if (starting_index >= length)
    {
        sender->round_robin_index = starting_index = 0;
    }

    for (size_t i = starting_index; i < length; i++)
    {
        int result = aeron_network_publication_send(publications[i].publication, now_ns);
        if (result < 0)
        {
            AERON_DRIVER_SENDER_ERROR(sender, "sender do_send: %s", aeron_errmsg());
        }
        else
        {
            bytes_sent += result;
        }
    }

    for (size_t i = 0; i < starting_index; i++)
    {
        int result = aeron_network_publication_send(publications[i].publication, now_ns);
        if (result < 0)
        {
            AERON_DRIVER_SENDER_ERROR(sender, "sender do_send: %s", aeron_errmsg());
        }
        else
        {
            bytes_sent += result;
        }
    }

    aeron_counter_add_ordered(sender->total_bytes_sent_counter, bytes_sent);

    return bytes_sent;
}
