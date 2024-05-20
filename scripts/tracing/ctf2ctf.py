#!/usr/bin/env python3
#
# Copyright (c) 2020 Intel Corporation.
# Copyright (c) 2023 Nordic Semiconductor ASA.
#
# SPDX-License-Identifier: Apache-2.0
import json
import sys
import datetime
import argparse
try:
    import bt2
except ImportError:
    sys.exit("Missing dependency: You need to install python bindings of babeltrace.")

def parse_args():
    parser = argparse.ArgumentParser(
            description=__doc__,
            formatter_class=argparse.RawDescriptionHelpFormatter, allow_abbrev=False)
    parser.add_argument("-t", "--trace",
            required=True,
            help="tracing data (directory with metadata and trace file)")
    args = parser.parse_args()
    return args

def add_metadata(name, pid, tid, args):
    return {
        'pid': pid,
        'tid': tid,
        'name': name,
        'ph': 'M',
        'cat': "__metadata",
        'args': args,
    }

g_thread_names = {}
g_buf_names = {}
active_buffers = {}
g_events = []
g_isr_active = False
NET_BUF_TID = 5

def spit_json(path, trace_events):
    trace_events.append(add_metadata("thread_name", 0, 0, {'name': 'general'}))
    trace_events.append(add_metadata("thread_name", 0, 1, {'name': 'ISR context'}))
    trace_events.append(add_metadata("thread_name", 0, 2, {'name': 'Mutex'}))
    trace_events.append(add_metadata("thread_name", 0, 3, {'name': 'Semaphore'}))
    trace_events.append(add_metadata("thread_name", 0, 4, {'name': 'Timer'}))

    for k in g_buf_names.keys():
        # TODO: group buffers in one PID per pool
        name = g_buf_names[k]['name']
        trace_events.append(add_metadata("thread_name", 1, int(k), {'name': f'NetBuf {name}'}))

    for k in g_thread_names.keys():
        trace_events.append(add_metadata("thread_name", 0, int(k), {'name': g_thread_names[k]['name']}))

    content = json.dumps({
        "traceEvents": trace_events,
        "displayTimeUnit": "ns"
    })

    with open(path, "w") as f:
        f.write(content)

def format_json(name, ts, ph, tid=0, meta=None, pid=0):
    # Chrome trace format
    # `args` has to have at least one arg, is
    # shown when clicking the event
    evt = {
        'pid': int(pid),
        'tid': int(tid),
        'name': name,
        'ph': ph,
        'ts': ts,
        'args': {
            'dummy': 0,
        },
    }

    if ph == 'X':
        # fake duration for now
        evt['dur'] = 10

    if meta is not None:
        evt['args'] = meta

    return evt

def add_thread(tid, name, active):
    if tid not in g_thread_names.keys():
        g_thread_names[tid] = {'name': str(name), 'active': active}
        return False

    prev = g_thread_names[tid]['active']
    g_thread_names[tid]['active'] = active

    return prev == active

def exit_isr(timestamp):
    # If a thread is switched in, we are no longer in ISR context
    # Generate a synthetic ISR end event
    g_isr_active = False
    g_events.append(format_json('isr_active', timestamp, 'E', 1))

def handle_thread_event(event, timestamp):
    if any(match in event.name for match in ['info', 'create', 'name_set']):
        tid = event.payload_field['thread_id']
        g_events.append(format_json(event.name, timestamp, 'i', 0))
        return

    if 'thread_switched_in' in event.name:
        ph = 'B'
    elif 'thread_switched_out' in event.name:
        ph = 'E'
    else:
        raise Exception(f'THREAD OTHER: {event.name}')

    tid = event.payload_field['thread_id']

    # Is this thread already running?
    already = add_thread(tid, event.payload_field['name'], ph == 'B')

    if already and ph == 'B':
        # Means the thread is already switched in/out,
        # adding another event will confuse the UI
        # It probably means that we are returning from an ISR
        print(f'Ignoring thread begin event for TID {hex(tid)}')
        return

    if ph == 'B' and g_isr_active:
        exit_isr(timestamp)

    g_events.append(format_json('running', timestamp, ph, tid, None))

def handle_buf_lifetime(event, timestamp):
    tid = NET_BUF_TID
    buf = event.payload_field['buf']
    poolname = event.payload_field['name']
    pool = event.payload_field['pool']

    if buf == 0:
        ph = 'i'
        free = event.payload_field['free']
        meta = {'pool_name': str(poolname), 'pool_addr': f'{hex(pool)}'}
        g_events.append(format_json(f"net_buf_alloc_failed", timestamp, ph, tid, meta))

    else:
        # Record pool free count
        ph = 'C'
        free = event.payload_field['free']
        meta = {f'{poolname} ({hex(pool)})': int(free)}
        g_events.append(format_json(f"free bufs", timestamp, ph, tid, meta, 1))

        # Record buffer lifetime as duration event
        if 'allocated' in event.name:
            ph = 'B'
            if buf in active_buffers.keys():
                raise Exception(f"Missing destroy for buf {hex(buf)}")
            # Store one ref. There is always one implicit ref when allocating a buffer.
            active_buffers[buf] = 1
        else:
            ph = 'E'
            if buf not in active_buffers.keys():
                raise Exception(f"Missing alloc for buf {hex(buf)}")
            del active_buffers[buf]

        meta = {'pool_name': str(poolname), 'pool_addr': f'{hex(pool)}'}
        g_events.append(format_json(f"buf [{hex(buf)}] in use", timestamp, ph, buf, meta, 1))

def handle_buf_event(event, timestamp):
    name = event.name
    tid = NET_BUF_TID

    if 'net_buf_allocated' in name or 'net_buf_destroyed' in name:
        handle_buf_lifetime(event, timestamp)

    elif 'net_buf_alloc' in name:
        ph = 'i'
        poolname = event.payload_field['name']
        pool = event.payload_field['pool']
        free = event.payload_field['free']
        meta = {'name': str(poolname), 'pool': f'{hex(pool)}', 'count': int(free)}
        g_events.append(format_json(name, timestamp, ph, tid, meta))

    elif 'ref' in name:
        buf = event.payload_field['buf']
        cnt = event.payload_field['count']
        tid = buf

        # TODO: keep track of origin pool and mention it in the name

        if tid not in g_buf_names.keys():
            g_buf_names[tid] = {'name': hex(buf), 'active': 0}

        if buf not in active_buffers.keys():
            raise Exception(f"Refcounting a buf that hasn't been allocated: {hex(buf)}")

        if 'unref' in name:
            ph = 'E'
            active_buffers[buf] -= 1
        else:
            ph = 'B'
            active_buffers[buf] += 1

        if cnt != active_buffers[buf]:
            print(f"Something doesn't add up: {hex(buf)}")

        # This will show up on the same thread as the "buf xx in use" event
        g_events.append(format_json(f"ref", timestamp, ph, tid, None, 1))

def handle_isr_event(event, timestamp):
    name = event.name
    global g_isr_active

    if 'isr_enter' in name:
        if g_isr_active:
            # print(f'Ignoring duplicate ISR enter (TS {timestamp} us)')
            return

        ph = 'B'
        g_isr_active = True

    elif 'isr_exit' in name:
        if not g_isr_active:
            # print(f'Ignoring duplicate ISR exit (TS {timestamp} us)')
            return

        ph = 'E'
        g_isr_active = False

    else:
        raise(Exception)

    # It's a bit sad, but we don't currently have that much info.
    # Adding the ISR vector number to zephyr's tracing would be a good start.
    name = 'isr_active'
    tid = 1

    g_events.append(format_json('isr_active', timestamp, ph, tid, None))

prev_evt_time_us = 0
def workaround_timing(evt_us):
    global prev_evt_time_us
    # workaround for UI getting confused when two events appear
    # at the same reported time. Especially 'B' and 'E' evts.
    # 1ns seems to be enough to make it work.
    if prev_evt_time_us >= evt_us:
        evt_us = prev_evt_time_us + 0.001

    prev_evt_time_us = evt_us

    return evt_us

def main():
    args = parse_args()

    msg_it = bt2.TraceCollectionMessageIterator(args.trace)
    timeline = []

    def do_trace(msg):
        # Timestamp is in microseconds, with nanosecond resolution
        timestamp = msg.default_clock_snapshot.ns_from_origin / 1000
        timestamp = workaround_timing(timestamp)

        # Setup default event data
        event = msg.event
        ph = 'i'
        name = event.name
        tid = 0
        meta = None

        if 'thread' in name:
            handle_thread_event(event, timestamp)
            return

        elif 'idle' in name:
            # Means no thread is switched in.
            # Also a valid way of exiting the ISR
            if g_isr_active:
                exit_isr(timestamp)

        elif 'isr' in name:
            handle_isr_event(event, timestamp)
            return

        elif 'mutex' in name:
            tid = 2

        elif 'semaphore' in name:
            tid = 3

        elif 'timer' in name:
            tid = 4

        elif 'net_buf' in name:
            handle_buf_event(event, timestamp)
            return

        else:
            raise Exception(f'Unknown event: {event.name} payload {event.payload_field}')

        # FIXME: move this next to the generated events
        g_events.append(format_json(name, timestamp, ph, tid, meta))

    try:
        for msg in msg_it:
            if not isinstance(msg, bt2._EventMessageConst):
                continue

            do_trace(msg)
    except bt2._Error as e:
        if e._msg != 'graph object could not run once':
            raise(e)
        print(f'Trace does not terminate cleanly')
    finally:
        spit_json('./out.json', g_events)
        print(f'Done')

if __name__=="__main__":
    main()
