#!/usr/bin/env python3
"""Local-only host viewer. The C game still renders every pixel and handles all rules.
Run `make host && python3 tools/host_viewer.py`, open http://127.0.0.1:8768.
"""
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
import json, subprocess, argparse, secrets
ROOT=Path(__file__).resolve().parents[1]
HTML='''<!doctype html><meta charset="utf-8"><title>Ink Wars host</title>
<style>body{background:white;color:black;font:16px monospace;margin:32px}canvas{width:888px;height:456px;image-rendering:pixelated;border:2px solid black;max-width:95vw}button{background:white;color:black;border:2px solid black;padding:10px;margin:4px}p{max-width:850px}</style>
<h1>墨纸战争 · 主机预览</h1><canvas width="296" height="152" id="screen"></canvas>
<p>方向键移动，Enter 确认，Escape / Backspace 取消。画面来自同一 C 程序的 1bit 帧缓冲，放大不插值。设备端只使用方向、OK/ENTER、BACK。</p>
<div><button data-k="w">上</button><button data-k="s">下</button><button data-k="a">左</button><button data-k="d">右</button><button data-k="o">确认</button><button data-k="b">取消</button></div><p id="status"></p>
<script>const token="TOKEN",c=document.querySelector('canvas'),ctx=c.getContext('2d');ctx.imageSmoothingEnabled=false;
async function key(k){await fetch('/key',{method:'POST',headers:{'X-InkWars':token},body:k});}
document.querySelectorAll('button').forEach(b=>b.onclick=()=>key(b.dataset.k));
addEventListener('keydown',e=>{const k={ArrowUp:'w',ArrowDown:'s',ArrowLeft:'a',ArrowRight:'d',Enter:'o',Escape:'b',Backspace:'b'}[e.key];if(k){e.preventDefault();if(!e.repeat)key(k);}});
async function update(){try{const r=await fetch('/frame',{cache:'no-store'});if(r.ok){const bytes=new Uint8Array(await r.arrayBuffer());let p=0,n=0;while(p<bytes.length&&n<2){if(bytes[p++]===10)n++;}if(bytes.length-p===5624){const im=ctx.createImageData(296,152);for(let i=0;i<296*152;i++){const v=(bytes[p+(i>>3)]&(128>>(i&7)))?0:255;im.data[i*4]=im.data[i*4+1]=im.data[i*4+2]=v;im.data[i*4+3]=255;}ctx.putImageData(im,0,0);}}const s=await(await fetch('/status')).json();document.querySelector('#status').textContent=s.running?'运行中 · 每次可见操作至少间隔 700ms':'游戏已退出；关闭网页后在终端按 Ctrl+C';}finally{setTimeout(update,700);}}update();</script>'''

def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--port',type=int,default=8768);args=parser.parse_args()
 token=secrets.token_hex(16);frame=ROOT/'build/host-live.pbm';save=ROOT/'build/host-live.sav'
 process=subprocess.Popen([str(ROOT/'build/inkwars-host'),'--preview',str(frame),'--save',str(save)],stdin=subprocess.PIPE,cwd=ROOT)
 class Handler(BaseHTTPRequestHandler):
  def log_message(self,*args): pass
  def respond(self,data,kind='text/plain',code=200):
   self.send_response(code);self.send_header('Content-Type',kind);self.send_header('Cache-Control','no-store');self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
  def do_GET(self):
   if self.path=='/':self.respond(HTML.replace('TOKEN',token).encode(),'text/html; charset=utf-8')
   elif self.path=='/frame':self.respond(frame.read_bytes() if frame.exists() else b'','image/x-portable-bitmap')
   elif self.path=='/status':self.respond(json.dumps({'running':process.poll() is None}).encode(),'application/json')
   else:self.respond(b'not found',code=404)
  def do_POST(self):
   if self.path!='/key' or self.headers.get('X-InkWars')!=token or self.headers.get('Content-Length')!='1':self.respond(b'forbidden',code=403);return
   key=self.rfile.read(1)
   if key not in (b'w',b's',b'a',b'd',b'o',b'b'):self.respond(b'bad key',code=400);return
   try:process.stdin.write(b'\n' if key==b'o' else key);process.stdin.flush();self.respond(b'ok')
   except BrokenPipeError:self.respond(b'exited',code=409)
 server=HTTPServer(('127.0.0.1',args.port),Handler)
 print(f'Ink Wars: http://127.0.0.1:{args.port}',flush=True)
 try:server.serve_forever()
 except KeyboardInterrupt:pass
 finally:
  server.server_close()
  if process.poll() is None:process.terminate();process.wait(timeout=5)
if __name__=='__main__':main()
