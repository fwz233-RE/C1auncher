#!/usr/bin/env python3
"""QEMU MIPS 运行器及跨平台屏幕界面，仅使用 Python 标准库。"""
import argparse
import base64
import collections
import http.server
import json
import os
import pathlib
import secrets
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
import urllib.error
import webbrowser

from repository import RepositoryError, configured

ROOT = pathlib.Path(__file__).resolve().parent
APPS = [{"id": "launcher", "name": "C1auncher", "version": "2.0.0"},
        {"id": "hello", "name": "Hello", "version": "1.5.1"},
        {"id": "book-reader", "name": "阅读器", "version": "0.1.20"},
        {"id": "pic", "name": "图片浏览器", "version": "0.1.4"},
        {"id": "chichugames", "name": "ChiChuGames", "version": "0.1.3"},
        {"id": "music-player", "name": "音乐播放器", "version": "0.3.4"},
        {"id": "pinao", "name": "Pinao", "version": "0.1.4"}]


class Simulator:
    def __init__(self):
        self.condition = threading.Condition()
        self.send_lock = threading.Lock()
        self.connection = None
        self.ready = False
        self.running = True
        self.app = "none"
        self.input_app = "none"
        self.pid = 0
        self.exit_code = None
        self.sequence = 0
        self.full = False
        self.pixels = bytes(5624)
        self.logs = collections.deque(maxlen=150)
        self.revision = 0
        self.token = secrets.token_urlsafe(32)
        self.console = None
        self.console_lock = threading.Lock()
        self.shutdown_done = threading.Event()

    def state(self):
        with self.condition:
            return {"ready": self.ready, "app": self.app, "input_app": self.input_app, "pid": self.pid, "exit_code": self.exit_code,
                    "sequence": self.sequence, "full": self.full, "apps": APPS, "logs": list(self.logs), "token": self.token}

    def frame(self):
        with self.condition:
            return {"sequence": self.sequence, "full": self.full, "pixels": base64.b64encode(self.pixels).decode()}

    def update(self):
        self.revision += 1
        self.condition.notify_all()

    def log(self, message):
        with self.condition:
            self.logs.append(message)
            self.update()

    def command(self, line):
        with self.send_lock:
            if not self.connection or not self.ready: raise ConnectionError("MIPS 客体尚未就绪")
            self.connection.sendall((line + "\n").encode())

    def receive(self, listener):
        try:
            connection, _ = listener.accept()
            with self.send_lock: self.connection = connection
            with connection, connection.makefile("rb") as stream:
                while self.running:
                    line = stream.readline(24000)
                    if not line: break
                    if not line.endswith(b"\n"): raise ValueError("客体消息过长")
                    fields = line.decode("ascii").strip().split(" ")
                    with self.condition:
                        if fields[0] == "READY": self.ready = True
                        elif fields[0] == "FRAME" and len(fields) == 4:
                            pixels = bytes.fromhex(fields[3])
                            if len(pixels) != 5624: raise ValueError("屏幕帧长度错误")
                            self.sequence, self.full, self.pixels = int(fields[1]), bool(int(fields[2])), pixels
                        elif fields[0] == "APP" and len(fields) == 4:
                            self.app, self.pid, self.exit_code = fields[1], int(fields[2]), None
                            self.input_app = self.app
                        elif fields[0] == "INPUT" and len(fields) == 2:
                            self.input_app = fields[1]
                        elif fields[0] == "EXIT" and len(fields) == 3:
                            self.exit_code, self.pid = int(fields[2]), 0
                            self.logs.append(f"{fields[1]} 退出，代码 {self.exit_code}")
                        elif fields[0] == "LOG" and len(fields) == 3:
                            self.logs.append(bytes.fromhex(fields[2]).decode("utf-8", errors="replace").rstrip())
                        elif fields[0] == "ERROR": self.logs.append("客体：" + " ".join(fields[1:]))
                        elif fields[0] == "BYE": self.shutdown_done.set()
                        self.update()
        except (OSError, ValueError) as error:
            with self.condition: self.logs.append(str(error))
        finally:
            with self.send_lock: self.connection = None
            with self.condition:
                self.ready = False
                self.update()


def handler(simulator, port, repository):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, format, *args): pass

        def reply(self, status, data, content_type="application/json; charset=utf-8"):
            if not isinstance(data, bytes): data = json.dumps(data, ensure_ascii=False).encode()
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self):
            path = urllib.parse.unquote(urllib.parse.urlsplit(self.path).path)
            if path == "/api/state": self.reply(200, simulator.state())
            elif path == "/api/frame": self.reply(200, simulator.frame())
            elif path == "/api/binaries": self.reply(200, (ROOT / "build/binaries.json").read_bytes())
            elif path == "/api/console":
                log = ROOT / "runtime/boot.log"
                self.reply(200, {"text": log.read_bytes()[-65536:].decode("utf-8", errors="replace") if log.exists() else ""})
            elif path == "/api/events": self.events()
            elif path.startswith("/repo/"):
                try:
                    data = repository.get(path[6:])
                except (RepositoryError, OSError) as error:
                    simulator.log("应用仓库：" + str(error))
                    self.reply(502, {"error": str(error)})
                else:
                    if data is not None: self.reply(200, data, "application/octet-stream")
                    else: self.reply(404, {"error": "文件不存在"})
            elif path in ["/", "/index.html", "/app.js", "/style.css"]:
                name = "index.html" if path == "/" else path[1:]
                types = {"html": "text/html", "js": "text/javascript", "css": "text/css"}
                self.reply(200, (ROOT / "public" / name).read_bytes(), types[name.rsplit(".", 1)[-1]] + "; charset=utf-8")
            else: self.reply(404, {"error": "页面不存在"})

        def events(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "close")
            self.end_headers()
            last_revision = -1
            last_sequence = -1
            try:
                while simulator.running:
                    with simulator.condition:
                        if last_revision == simulator.revision: simulator.condition.wait(timeout=15)
                        revision = simulator.revision
                        state = simulator.state()
                        frame = simulator.frame() if last_sequence != simulator.sequence else None
                    self.wfile.write(("event: state\ndata: " + json.dumps(state, ensure_ascii=False) + "\n\n").encode())
                    if frame:
                        self.wfile.write(("event: frame\ndata: " + json.dumps(frame) + "\n\n").encode())
                        last_sequence = frame["sequence"]
                    self.wfile.flush()
                    last_revision = revision
            except (BrokenPipeError, ConnectionResetError): pass
            self.close_connection = True

        def do_POST(self):
            origin = self.headers.get("Origin")
            if origin and origin not in [f"http://127.0.0.1:{port}", f"http://localhost:{port}"]:
                self.reply(403, {"error": "来源不允许"}); return
            if self.headers.get("X-C1Sim-Token") != simulator.token:
                self.reply(403, {"error": "会话令牌无效"}); return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if length < 1 or length > 4096: raise ValueError("请求长度错误")
                data = json.loads(self.rfile.read(length))
                if self.path == "/api/key":
                    code, value, source = data["code"], data["value"], data.get("source", 0)
                    if any(type(v) is not int for v in [code, value, source]) or not (0 <= code <= 767 and value in [0, 1, 2] and source in [0, 1]):
                        raise ValueError("按键参数错误")
                    simulator.command(f"KEY {source} {code} {value}")
                elif self.path == "/api/run":
                    if data.get("id") not in [app["id"] for app in APPS]: raise ValueError("应用不存在")
                    simulator.command("RUN " + data["id"])
                elif self.path == "/api/battery":
                    percent, plugged = data["percent"], data["plugged"]
                    if type(percent) is not int or not 0 <= percent <= 100 or type(plugged) is not bool:
                        raise ValueError("电池参数错误")
                    simulator.command(f"BATTERY {percent} {int(plugged)}")
                elif self.path == "/api/stop": simulator.command("STOP")
                elif self.path == "/api/shutdown":
                    self.reply(200, {"ok": True})
                    threading.Thread(target=self.server.shutdown, daemon=True).start()
                    return
                elif self.path == "/api/console":
                    text = data["text"]
                    if not isinstance(text, str) or len(text) > 2048: raise ValueError("串口输入长度错误")
                    if not simulator.console: raise ConnectionError("QEMU 串口尚未就绪")
                    with simulator.console_lock:
                        simulator.console.write(text.encode()); simulator.console.flush()
                else: self.reply(404, {"error": "接口不存在"}); return
                self.reply(200, {"ok": True})
            except (ValueError, KeyError, TypeError) as error: self.reply(400, {"error": str(error)})
            except OSError as error: self.reply(503, {"error": str(error)})
    return Handler


def audio_arguments(qemu, backend, recording):
    if backend == "none":
        if recording:
            raise ValueError("--audio-record 不能与静音后端同时使用")
        return [], backend
    result = subprocess.run([qemu, "-audiodev", "help"], capture_output=True, text=True, check=True)
    available = set(line.strip() for line in (result.stdout + result.stderr).splitlines())
    if recording:
        if backend not in ("auto", "wav"): raise ValueError("--audio-record 不能与其他音频后端同时使用")
        backend = "wav"
        if recording.exists(): raise ValueError("录音文件已存在，请选择新的 --audio-record 路径")
        recording.parent.mkdir(parents=True, exist_ok=True)
    if backend == "auto":
        preferences = ["coreaudio"] if sys.platform == "darwin" else ["dsound", "sdl"] if os.name == "nt" else ["pipewire", "pa", "alsa", "sdl"]
        backend = next((name for name in preferences if name in available), None)
        if not backend: raise ValueError("当前 QEMU 没有可用的宿主音频后端；可显式使用 --audio-backend none 静音运行")
    if backend not in available: raise ValueError(f"当前 QEMU 不支持音频后端 {backend}")
    if backend == "wav" and not recording: raise ValueError("wav 后端需要 --audio-record 路径")
    device = f"{backend},id=audio0,out.frequency=48000,out.channels=2,out.format=s16"
    if recording: device += ",path=" + str(recording.resolve()).replace(",", ",,")
    return ["-audiodev", device, "-device", "AC97,audiodev=audio0"], backend


def finish_recording(path):
    # QEMU's wav backend can leave the two RIFF size fields at zero even
    # after a QMP quit. Preserve its PCM bytes and finalize the container.
    if not path.exists(): return
    with path.open("r+b") as recording:
        header = recording.read(44)
        if len(header) != 44 or header[:4] != b"RIFF" or header[8:16] != b"WAVEfmt " or header[36:40] != b"data":
            raise ValueError("QEMU 输出不是预期的 PCM WAV 文件")
        size = recording.seek(0, os.SEEK_END)
        if (size - 44) % 4: raise ValueError("QEMU WAV 输出包含不完整的 PCM 帧")
        recording.seek(4); recording.write(struct.pack("<I", size - 8))
        recording.seek(40); recording.write(struct.pack("<I", size - 44))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--no-open", action="store_true")
    parser.add_argument("--stop", action="store_true", help="正常关闭指定端口的现有模拟器")
    parser.add_argument("--disk", type=pathlib.Path)
    parser.add_argument("--gdb", type=int, help="QEMU 客体调试端口，只监听本机")
    parser.add_argument("--binary", type=pathlib.Path, help="加载自己的原始静态 MIPS ELF 文件")
    parser.add_argument("--resources", type=pathlib.Path, help="自定义程序的资源目录")
    parser.add_argument("--core-dir", type=pathlib.Path, help="运行本地编译的四个核心组件，例如 ../C1ancher/build")
    parser.add_argument("--audio-backend", default="auto", choices=["auto", "coreaudio", "pipewire", "pa", "alsa", "dsound", "sdl", "none", "wav"], help="QEMU 宿主音频后端，默认自动选择")
    parser.add_argument("--audio-record", type=pathlib.Path, help="将虚拟声卡输出保存到新的 WAV 文件，供验证使用")
    args = parser.parse_args()
    if not 1 <= args.port <= 65535: parser.error("端口必须为 1–65535")
    if args.stop:
        url = f"http://127.0.0.1:{args.port}"
        try:
            with urllib.request.urlopen(url + "/api/state", timeout=5) as response:
                token = json.load(response)["token"]
            request = urllib.request.Request(url + "/api/shutdown", b"{}",
                                             {"Content-Type": "application/json", "X-C1Sim-Token": token})
            with urllib.request.urlopen(request, timeout=5) as response: json.load(response)
            for _ in range(50):
                try:
                    with urllib.request.urlopen(url + "/api/state", timeout=1): pass
                except (OSError, urllib.error.URLError): break
                time.sleep(0.1)
            else: raise OSError("关闭超时，请查看启动终端和 runtime/boot.log")
            print("模拟器已关闭。")
        except OSError as error: parser.error("关闭模拟器失败：" + str(error))
        return
    if args.resources and not args.binary: parser.error("--resources 需要配合 --binary")
    qemu = shutil.which("qemu-system-mipsel")
    qemu_img = shutil.which("qemu-img")
    if not qemu or not qemu_img: parser.error("请先安装 QEMU，参见 README.md")
    try: audio_command, audio_backend = audio_arguments(qemu, args.audio_backend, args.audio_record)
    except (ValueError, subprocess.CalledProcessError) as error: parser.error(str(error))
    for name in ["vmlinux", "rootfs.cpio.gz"]:
        if not (ROOT / "build/images" / name).is_file(): parser.error("请先执行 python3 scripts/build.py")
    if args.binary or args.core_dir:
        sys.path.insert(0, str(ROOT / "scripts"))
        from pack_guest import main as pack
        try: pack(args.binary, args.resources, args.core_dir)
        except (OSError, ValueError, struct.error) as error: parser.error("导入设备程序失败：" + str(error))
    audit = json.loads((ROOT / "build/binaries.json").read_text())
    versions = {row["id"]: row["version"] for row in audit}
    for app in APPS:
        app["version"] = versions.get("c1ancher" if app["id"] == "launcher" else app["id"], app["version"])
    local_build = any(row.get("source_type") == "local-core" for row in audit)
    if args.disk is None:
        args.disk = ROOT / "runtime" / ("dev-data.qcow2" if local_build else "data.qcow2")
    if any(row["id"] == "custom" for row in audit):
        APPS.append({"id": "custom", "name": "自己的 MIPS 程序", "version": "local"})
    simulator = Simulator()
    try: repository = configured(ROOT, simulator.log)
    except (RepositoryError, OSError, ValueError) as error: parser.error(str(error))
    (ROOT / "runtime").mkdir(exist_ok=True)
    args.disk.parent.mkdir(parents=True, exist_ok=True)
    new_disk = not args.disk.exists()
    if new_disk: subprocess.run([qemu_img, "create", "-f", "qcow2", str(args.disk), "256M"], check=True)
    listener = socket.socket()
    listener.bind(("127.0.0.1", 0)); listener.listen(1)
    bridge_port = listener.getsockname()[1]
    # QEMU connects to this private listener; no management port is exposed.
    qmp_listener = socket.socket()
    qmp_listener.bind(("127.0.0.1", 0)); qmp_listener.listen(1); qmp_listener.settimeout(5)
    qmp_port = qmp_listener.getsockname()[1]
    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), handler(simulator, args.port, repository))
    server.daemon_threads = True
    threading.Thread(target=simulator.receive, args=(listener,), daemon=True).start()
    command = [qemu, "-M", "malta", "-cpu", "24Kf", "-m", "256", "-kernel", str(ROOT / "build/images/vmlinux"),
               "-initrd", str(ROOT / "build/images/rootfs.cpio.gz"),
               "-append", f"console=ttyS0 rdinit=/init c1sim_port={bridge_port} c1sim_http={args.port} c1sim_newdisk={int(new_disk)}",
               "-display", "none", "-serial", "stdio", "-monitor", "none", "-no-reboot",
               "-qmp", f"tcp:127.0.0.1:{qmp_port}",
               "-netdev", "user,id=net0", "-device", "pcnet,netdev=net0",
               "-drive", f"file={args.disk.resolve()},format=qcow2,if=none,id=data", "-device", "virtio-blk-pci,drive=data"]
    command += audio_command
    if args.gdb:
        if not 1 <= args.gdb <= 65535: parser.error("GDB 端口必须为 1–65535")
        command += ["-gdb", f"tcp:127.0.0.1:{args.gdb}"]
    url = f"http://127.0.0.1:{args.port}"
    process = None
    qmp = None
    boot_log = (ROOT / "runtime/boot.log").open("wb")
    def stop_signal(*_): raise KeyboardInterrupt()
    signal.signal(signal.SIGTERM, stop_signal)
    if hasattr(signal, "SIGHUP"): signal.signal(signal.SIGHUP, stop_signal)
    try:
        process_options = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == "nt" else {"start_new_session": True}
        process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=boot_log, stderr=subprocess.STDOUT, **process_options)
        qmp, _ = qmp_listener.accept()
        qmp.sendall(b'{"execute":"qmp_capabilities"}\r\n')
        qmp_listener.close()
        simulator.console = process.stdin
        print(f"C1-Slim 模拟器：{url}\n原始 MIPS 二进制 · QEMU PID {process.pid} · 音频 {audio_backend}\nCtrl+C 退出；数据保存在 {args.disk}", flush=True)
        def monitor():
            code = process.wait()
            if simulator.running:
                with simulator.condition:
                    simulator.ready = False
                    simulator.logs.append(f"QEMU 已退出，代码 {code}。查看 runtime/boot.log。")
                    simulator.update()
                server.shutdown()
        threading.Thread(target=monitor, daemon=True).start()
        if not args.no_open: webbrowser.open(url)
        server.serve_forever(poll_interval=0.25)
    except KeyboardInterrupt: pass
    finally:
        if simulator.ready and process and process.poll() is None:
            try:
                simulator.command("SHUTDOWN")
                simulator.shutdown_done.wait(timeout=3)
            except OSError: pass
        simulator.running = False
        with simulator.condition: simulator.condition.notify_all()
        if process and process.poll() is None:
            try:
                if not qmp: raise OSError("QMP unavailable")
                qmp.sendall(b'{"execute":"quit"}\r\n')
                process.wait(timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired: process.kill(); process.wait()
        if qmp: qmp.close()
        qmp_listener.close()
        listener.close(); server.server_close()
        if process and process.stdin: process.stdin.close()
        boot_log.close()
        if args.audio_record: finish_recording(args.audio_record)


if __name__ == "__main__": main()
