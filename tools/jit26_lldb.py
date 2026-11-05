"""lldb helper implementing the StikDebug "universal" JIT protocol (TXM devices, iOS 26+).

The app executes `brk #0xf00d` with x16 = command:
  1 = prepare region (x0 = address or 0, x1 = length) -> bless every 16 KiB page by
      writing one byte through the debugger, return the address in x0
  0 = detach

usage (inside lldb attached to the app):
  command script import tools/jit26_lldb.py
  jit26        # resumes and services brk #0xf00d requests until the app asks to detach
"""
import lldb

PAGE = 16384


def _service(debugger, command, result, internal_dict):
    target = debugger.GetSelectedTarget()
    process = target.GetProcess()
    debugger.SetAsync(False)
    while True:
        process.Continue()
        state = process.GetState()
        if state in (lldb.eStateExited, lldb.eStateDetached, lldb.eStateCrashed):
            print("jit26: process ended", state)
            return
        thread = next((t for t in process if t.GetStopReason() in
                       (lldb.eStopReasonException, lldb.eStopReasonSignal, lldb.eStopReasonBreakpoint)), None)
        if thread is None:
            continue
        frame = thread.GetFrameAtIndex(0)
        pc = frame.GetPC()
        err = lldb.SBError()
        insn = process.ReadUnsignedFromMemory(pc, 4, err)
        if err.Fail() or (insn & 0xFFE0001F) != 0xD4200000 or ((insn >> 5) & 0xFFFF) != 0xF00D:
            # Not ours: pass the signal/exception on.
            print("jit26: unrelated stop at 0x%x (%s)" % (pc, thread.GetStopDescription(100)))
            continue
        regs = frame.GetRegisters().GetFirstValueByName("General Purpose Registers")
        x16 = regs.GetChildMemberWithName("x16").GetValueAsUnsigned()
        x0 = regs.GetChildMemberWithName("x0").GetValueAsUnsigned()
        x1 = regs.GetChildMemberWithName("x1").GetValueAsUnsigned()
        frame.SetPC(pc + 4)
        if x16 == 0:
            print("jit26: app requested detach")
            process.Detach()
            return
        if x16 == 1:
            if x0 == 0:
                print("jit26: allocation requests are not supported; app must pass an address")
                continue
            pages = (x1 + PAGE - 1) // PAGE
            print("jit26: blessing %d pages at 0x%x" % (pages, x0))
            for i in range(pages):
                process.WriteMemory(x0 + i * PAGE, b"\x69", err)
                if err.Fail():
                    print("jit26: write failed at page %d: %s" % (i, err))
                    break
            frame.FindRegister("x0").SetValueFromCString(hex(x0))
            continue


def __lldb_init_module(debugger, internal_dict):
    debugger.HandleCommand("command script add -f jit26_lldb._service jit26")
    debugger.HandleCommand("process handle SIGTRAP -s true -p false -n false")
