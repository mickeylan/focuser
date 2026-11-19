#!/usr/bin/env python3
"""DIY two-point calibration buttons. Close ASCOM/Alpaca before opening the port.

python host/focus_calibrate.py --port COM7
No automatic motion, zeroing or calibration occurs on startup.
"""
import argparse
import concurrent.futures
import tkinter as tk
from tkinter import messagebox
from focus_bridge import Focuser, SerialTransport, SocketTransport


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--port")
    group.add_argument("--tcp", metavar="HOST:PORT")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()
    root = tk.Tk()
    root.title("DIY 调焦行程标定")
    # Explicit user-selected transport only. Opening the window sends no movement.
    if args.tcp:
        host, port = args.tcp.rsplit(":", 1)
        transport = SocketTransport(host, int(port))
    else:
        transport = SerialTransport(args.port, args.baud)
    focuser = Focuser(transport)
    executor = concurrent.futures.ThreadPoolExecutor(max_workers=1)
    pending = None
    closing = False
    busy = False
    label = tk.StringVar(value="正在读取设备状态…")
    tk.Label(root, text="先关闭 ASCOM/Alpaca，避免抢占串口。\n标定时没有外端保护，请观察机构，接近尽头用 1 步。", fg="red").pack(padx=15, pady=10)
    tk.Label(root, textvariable=label).pack(pady=8)
    buttons = []
    jog_buttons = []

    def submit(call):
        nonlocal pending, busy
        if busy or closing:
            return
        busy = True
        for button in buttons:
            button.config(state="disabled")
        def work():
            call()
            return focuser.limits(), focuser.status()
        pending = executor.submit(work)

    def confirmed(call, text):
        if not busy and messagebox.askyesno("确认", text, parent=root):
            submit(call)

    def button(text, call, parent=root):
        widget = tk.Button(parent, text=text, command=call, width=24)
        widget.pack(padx=8, pady=3)
        buttons.append(widget)
        return widget

    zero = button("确认当前为零位（保留最大行程）", lambda: confirmed(focuser.confirm_zero,
                  "已手动放在安全内收零位，并预留余量？\n只修改坐标，不会自动寻找原点。"))
    begin = button("重新标定：设当前为零位", lambda: confirmed(focuser.begin_calibration,
                   "已将机构放到安全内收位置？\n将进入无已知外端限制的人工点动标定，旧记录只有保存新最大位后才替换。"))
    frame = tk.Frame(root)
    frame.pack()
    for steps in (-100, -10, -1, 1, 10, 100):
        jog_buttons.append(button(f"{'内收' if steps < 0 else '外伸'} {abs(steps)} 步",
                                  lambda n=steps: submit(lambda: focuser.jog(n)), frame))
    maximum = button("记录当前位置为最大位并保存", lambda: confirmed(focuser.save_maximum,
                     "已停止，当前位置距离机械外端留有安全余量？\n保存后最大行程为当前累计步数。"))
    # Separate stop: no movement backlog is queued. Runs after at most one short
    # request currently in flight; wire latency means this is not a hardware E-stop.
    def halt():
        if not closing:
            executor.submit(focuser.halt)
    tk.Button(root, text="停止 HALT", command=halt, fg="red", width=24).pack(pady=8)

    def poll():
        nonlocal pending, busy
        if closing:
            return
        if pending is None:
            submit(lambda: None)
        elif pending.done():
            try:
                limits, status = pending.result()
                states = {0: "请先回到零位并确认", 1: "人工点动标定（无已知外端限位）", 2: "已标定，软限位生效"}
                label.set(f"{states[limits['state']]}\n当前位置 {status['pos']} 步，最大 {limits['max']} 步\n"
                          f"{'运动中' if status['moving'] else '已停止'}")
                for b in buttons:
                    b.config(state="normal" if not status['moving'] else "disabled")
                for b in jog_buttons:
                    b.config(state="normal" if limits['state'] == 1 and not status['moving'] else "disabled")
                maximum.config(state="normal" if limits['state'] == 1 and status['pos'] > 0 and not status['moving'] else "disabled")
            except Exception as error:
                label.set(f"操作失败：{error}\n请检查连接；存储失败不会显示为保存成功。")
                for b in buttons:
                    b.config(state="normal")
            pending = None
            busy = False
        root.after(200, poll)

    def close():
        nonlocal closing
        if closing:
            return
        closing = True
        root.withdraw()
        def cleanup():
            try:
                focuser.halt()
            finally:
                focuser.close()
        future = executor.submit(cleanup)
        def finish():
            if not future.done():
                root.after(100, finish)
                return
            executor.shutdown(wait=False)
            root.destroy()
        finish()
    root.protocol("WM_DELETE_WINDOW", close)
    root.after(0, poll)
    try:
        root.mainloop()
    finally:
        executor.shutdown(wait=True)
        focuser.close()


if __name__ == "__main__":
    main()
