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
    trace_events.append(add_metadata("thread_name", 0, 5, {'name': 'Net'}))
    trace_events.append(add_metadata("thread_name", 0, 6, {'name': 'Socket'}))
    trace_events.append(add_metadata("thread_name", 0, 7, {'name': 'Custom'}))

    for k in g_thread_names.keys():
        trace_events.append(add_metadata("thread_name", 0, int(k), {'name': g_thread_names[k]['name']}))

    content = json.dumps({
        "traceEvents": trace_events,
        "displayTimeUnit": "ns"
    })

    with open(path, "w") as f:
        f.write(content)

def format_json(name, ts, ph, tid=0, meta=None, pid=0, args=None):
    # Chrome trace format
    # `args` has to have at least one arg, is
    # shown when clicking the event
    if args is None:
        json_args = {'dummy': 0}
    else:
        json_args = args
    evt = {
        'pid': int(pid),
        'tid': int(tid),
        'name': name,
        'ph': ph,
        'ts': ts,
        'args': json_args,
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
    if any(match in event.name for match in ['info', 'create', 'name_set',
        'wakeup', 'priority_set', 'pending']):
        tid = event.payload_field['thread_id']
        g_events.append(format_json(event.name, timestamp, 'i', tid))
        return

    if any(match in event.name for match in ['thread_switched_in',
                                             'thread_resume']):
        ph = 'B'
    elif any(match in event.name for match in ['thread_switched_out',
                                             'thread_abort',
                                             'thread_suspend']):
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

def handle_gpio_event(event, timestamp):
    # One-time event
    if any(match in event.name for match in ['file_callback']):
        tid = int(event.payload_field['port'])
        g_events.append(format_json(event.name, timestamp, 'i', 0))
        return

    if "_enter" in event.name:
        ph = 'B'
    elif "_exit" in event.name:
        ph = 'E'
    else:
        raise Exception(f'THREAD OTHER: {event.name}')

    tid = int(event.payload_field['port'])

    # Is this thread already running?
    already = add_thread(tid,
                         "GPIO{:02x}".format(int(event.payload_field['port'])), ph == 'B')

    if already and ph == 'B':
        # Means the thread is already switched in/out,
        # adding another event will confuse the UI
        # It probably means that we are returning from an ISR
        print(f'Ignoring thread begin event for TID {hex(tid)}')
        return

    if ph == 'B' and g_isr_active:
        exit_isr(timestamp)

    status = event.name.replace("gpio_", "")
    status = status.replace("_enter", "")
    status = status.replace("_exit", "")
    args = {}
    for key, val in event.payload_field.items():
        if key == "port":
            args[key] = "0x{:02X}".format(int(val))
        else:
            args[key] = int(val)
    g_events.append(format_json(status, timestamp, ph, tid, None,
                                args=args))

def handle_semaphore_event(event, timestamp):
    if any(match in event.name for match in ['take_blocking', "take_enter",
           "give_enter"]):
        tid = event.payload_field['id']
        args = {}
        for key, val in event.payload_field.items():
            args[key] = int(val)
        g_events.append(format_json(event.name, timestamp, 'i', tid, args=args))
        return

    if any(match in event.name for match in ['take_exit']):
        ph = 'B'
    elif any(match in event.name for match in ['give_exit', 'reset']):
        ph = 'E'
    else:
        raise Exception(f'THREAD OTHER: {event.name}')

    tid = event.payload_field['id']

    # Is this thread already running?
    already = add_thread(tid, "Semaphore {:02x}".format(int(event.payload_field['id'])),
                         ph == 'B')

    if already and ph == 'B':
        # Means the thread is already switched in/out,
        # adding another event will confuse the UI
        # It probably means that we are returning from an ISR
        print(f'Ignoring sema begin event for TID {hex(tid)}')
        return

    g_events.append(format_json('taken', timestamp, ph, tid, None))

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

def handle_named_event(event, timestamp):
    name = event.name

    args = {}
    for key, val in event.payload_field.items():
        if key != "name":
            args[key] = int(val)
    g_events.append(format_json(str(event.payload_field["name"]), timestamp,
                                'i', 7, None,
                                args=args))

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

        elif 'named_event' in name:
            handle_named_event(event, timestamp)
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
            handle_semaphore_event(event, timestamp)
            return

        elif 'timer' in name:
            tid = 4

        elif 'gpio' in name:
            handle_gpio_event(event, timestamp)
            return

        elif 'net' in name:
            tid = 5

        elif 'socket' in name:
            tid = 6

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
