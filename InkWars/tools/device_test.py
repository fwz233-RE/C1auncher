#!/usr/bin/env python3
"""Push and run bounded tests on exactly one authorized ADB device; no reboot.
Leaves C1ancher running to expose rather than hide display ownership conflicts.
"""
from pathlib import Path
import argparse, os, subprocess, datetime
ROOT=Path(__file__).resolve().parents[1]
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--adb',default=os.environ.get('ADB',str(Path(os.environ.get('LOCALAPPDATA',''))/'Android/Sdk/platform-tools/adb.exe') if os.name=='nt' else 'adb'))
 a=p.parse_args();log=[]
 def run(args,timeout=120):
  r=subprocess.run([a.adb]+args,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=timeout)
  text=r.stdout.decode('utf-8',errors='replace').replace('\r','');print(text,end='',flush=True);log.append(text)
  if r.returncode:raise RuntimeError(f'ADB command failed ({r.returncode})')
  return text
 devices=run(['devices']);ids=[l.split()[0] for l in devices.splitlines() if len(l.split())==2 and l.split()[1]=='device']
 if len(ids)!=1:raise SystemExit('Exactly one authorized device is required')
 stage=ROOT/'build/device-smoke-lf.sh'
 stage.write_bytes((ROOT/'tests/device-smoke.sh').read_bytes().replace(b'\r\n',b'\n'))
 report=ROOT/'build/device-test.log';log.append('UTC '+datetime.datetime.now(datetime.timezone.utc).isoformat()+'\n')
 try:
  run(['shell','mkdir -p /usr/data/inkwars-test'])
  for name in ['inkwars','test_game-mips','test_ui-mips']:
   run(['push',str(ROOT/'build'/name),'/usr/data/inkwars-test/'+name])
  run(['push',str(stage),'/usr/data/inkwars-test/device-smoke.sh'])
  result=run(['shell','chmod 755 /usr/data/inkwars-test/inkwars /usr/data/inkwars-test/test_game-mips /usr/data/inkwars-test/test_ui-mips; sh /usr/data/inkwars-test/device-smoke.sh'],timeout=180)
  if 'DEVICE_SMOKE_PASS' not in result:raise RuntimeError('Missing successful device marker')
  (ROOT/'build/device-previews').mkdir(exist_ok=True)
  host_frames=sorted((ROOT/'build/previews').glob('[0-9][0-9]-*.pbm'))
  if len(host_frames)!=15:raise RuntimeError('Expected exactly 15 host comparison frames')
  for host in host_frames:
   device=ROOT/'build/device-previews'/host.name
   run(['pull','/usr/data/inkwars-test/previews/'+host.name,str(device)])
   if host.read_bytes()!=device.read_bytes():raise RuntimeError('Host/device PBM mismatch: '+host.name)
  log.append('PASS: all 15 host/device PBM files byte-identical. These are generated frames, not panel photographs.\n')
  print(log[-1],end='')
 finally:report.write_text(''.join(log),encoding='utf-8')
if __name__=='__main__':main()
