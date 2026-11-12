"""Host-only LLDB implementation. No expressions or target code execution."""
import json
import os
from pathlib import Path
import signal
import time


def file_path(spec):
    return os.path.join(spec.GetDirectory() or "", spec.GetFilename() or "")


def snapshot(process, elapsed, app_uuids):
    threads = []
    for thread in process:
        frames = []
        for index in range(20):
            frame = thread.GetFrameAtIndex(index)
            if not frame.IsValid():
                break
            module = frame.GetModule()
            address = frame.GetPCAddress()
            uuid = (module.GetUUIDString() or "").upper() if module.IsValid() else ""
            frames.append(dict(pc=hex(frame.GetPC()),
                               function=frame.GetFunctionName() if uuid in app_uuids else None,
                               image=file_path(module.GetFileSpec()) if module.IsValid() else None,
                               image_uuid=module.GetUUIDString() if module.IsValid() else None,
                               file_address=hex(address.GetFileAddress())))
        threads.append(dict(id=thread.GetThreadID(), index=thread.GetIndexID(),
                            name=thread.GetName(), stop_reason=int(thread.GetStopReason()),
                            frames=frames))
    return dict(elapsed_seconds=elapsed, stop_id=process.GetStopID(), threads=threads)


def run(debugger):
    import lldb
    config = json.loads(Path(os.environ['CARA_STACK_SAMPLE_CONFIG']).read_text())
    report = dict(config=config, samples=[], detached=False)
    process = None
    listener = None
    report['state_events'] = []
    old_async = debugger.GetAsync()
    previous_alarm = signal.getsignal(signal.SIGALRM)
    def timeout(_signal, _frame):
        raise TimeoutError('Host sampling deadline exceeded')
    signal.signal(signal.SIGALRM, timeout)
    signal.alarm(80)
    deadline = time.monotonic() + 75
    def state_name(state):
        return lldb.SBDebugger.StateAsCString(state) or str(state)
    def pump_events():
        # The debugger's primary queue also needs draining: event removal can
        # advance LLDB's public process state. The private listener records it.
        for source in (debugger.GetListener(), listener):
            if source is None:
                continue
            event = lldb.SBEvent()
            while source.GetNextEvent(event):
                if lldb.SBProcess.EventIsProcessEvent(event):
                    state = lldb.SBProcess.GetStateFromEvent(event)
                    report['state_events'].append(dict(state=int(state),
                        name=state_name(state), host_monotonic=time.monotonic()))
                event = lldb.SBEvent()
    def wait_stopped(timeout_seconds):
        end = min(deadline, time.monotonic() + timeout_seconds)
        while time.monotonic() < end:
            pump_events()
            state = process.GetState()
            report['last_process_state'] = dict(value=int(state), name=state_name(state))
            if state == lldb.eStateStopped:
                return
            if state in (lldb.eStateExited, lldb.eStateDetached, lldb.eStateCrashed,
                         lldb.eStateInvalid):
                raise RuntimeError('Unexpected process state: ' + state_name(state))
            time.sleep(0.01)
        raise TimeoutError('Process did not stop within ' + str(timeout_seconds) +
                           ' seconds; actual state=' + state_name(process.GetState()))
    try:
        target = debugger.GetSelectedTarget()
        process = target.GetProcess()
        if not process.IsValid() or process.GetProcessID() != config['pid']:
            raise RuntimeError('Attached process identity mismatch')
        listener = lldb.SBListener('cara-stack-sampler')
        listener.StartListeningForEvents(process.GetBroadcaster(),
                                         lldb.SBProcess.eBroadcastBitStateChanged)
        wait_stopped(20)
        images = [dict(path=file_path(m.GetFileSpec()), uuid=m.GetUUIDString(),
                       loaded_header=m.GetObjectFileHeaderAddress().GetLoadAddress(target))
                  for m in target]
        report['images'] = images
        wanted = set(config['binary_uuids'])
        if not any((m.get('uuid') or '').upper() in wanted and
                   m['loaded_header'] != lldb.LLDB_INVALID_ADDRESS for m in images):
            raise RuntimeError('Attached images do not match preserved executable UUID')
        debugger.SetAsync(True)
        started = time.monotonic()
        end = min(deadline, started + config['seconds'])
        while time.monotonic() < end:
            error = process.Continue()
            if error.Fail():
                raise RuntimeError('Continue failed: ' + str(error))
            time.sleep(min(config['interval'], max(0, end - time.monotonic())))
            error = process.Stop()
            if error.Fail():
                raise RuntimeError('Stop failed: ' + str(error))
            wait_stopped(5)
            report['samples'].append(snapshot(process, time.monotonic() - started, wanted))
        report['elapsed_seconds'] = time.monotonic() - started
    except Exception as error:
        report['error'] = str(error)
    finally:
        signal.alarm(0)
        # Preserve cleanup if sampling fails before process validation.
        if process is None or not process.IsValid():
            process = debugger.GetSelectedTarget().GetProcess()
        if process.IsValid():
            if process.GetState() == lldb.eStateDetached:
                report['detached'] = True
            else:
                error = process.Detach(False)  # Explicitly resume on detach.
                report['detached'] = error.Success()
                report['detach_error'] = str(error) if error.Fail() else None
        debugger.SetAsync(old_async)
        signal.signal(signal.SIGALRM, previous_alarm)
        Path(config['output'], 'stacks.json').write_text(json.dumps(report, indent=2) + '\n')
