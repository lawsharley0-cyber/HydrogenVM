#!/usr/bin/env python3
"""Capture a version-pinned clickable UI plan in the ordinary emulator.

No backend/network flags, save reset, automatic failure retries or inferred
visual passes. Review per-step screenshots against the expected behavior.
Use a private QA copy when the plan spends resources or changes progression.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import time
import socket
import threading
import wave
import array
import sys


class CapturedAudio:
    def __init__(self,path,began):
        self.reader,self.writer=socket.socketpair(socket.AF_UNIX,socket.SOCK_DGRAM)
        if sys.platform=='darwin':
            self.writer.setsockopt(socket.SOL_SOCKET,socket.SO_SNDBUF,16384)
            self.reader.setsockopt(socket.SOL_SOCKET,socket.SO_RCVBUF,(8192+16)*2)
        self.reader.settimeout(.1);self.done=threading.Event();self.samples=[];self.error=None
        def pump():
            try:
                with wave.open(str(path),'wb') as recording:
                    recording.setparams((2,2,44100,0,'NONE','not compressed'))
                    while not self.done.is_set():
                        try:data=self.reader.recv(8193)
                        except socket.timeout:continue
                        if len(data)!=8192:raise ValueError('Invalid mixer block')
                        recording.writeframesraw(data);pcm=array.array('h',data)
                        if sys.byteorder!='little':pcm.byteswap()
                        self.samples.append((time.monotonic()-began,max(abs(min(pcm)),abs(max(pcm)))))
            except Exception as error:self.error=str(error)
        self.thread=threading.Thread(target=pump,name='ui-audit-audio');self.thread.start()

    def window(self,start,end):
        peaks=[peak for at,peak in self.samples if start<=at<=end]
        return dict(blocks=len(peaks),peak=max(peaks) if peaks else None)

    def close(self):
        self.done.set();self.thread.join(timeout=2);self.reader.close();self.writer.close()


def capture(frame, destination):
    from PIL import Image
    for _ in range(40):
        if frame.exists():
            data=frame.read_bytes()
            if len(data)>=16:
                magic,width,height,scale=struct.unpack_from('<4sIIf',data)
                if magic==b'HVF1' and 0<width<=8192 and 0<height<=8192 and len(data)==16+width*height*4:
                    Image.frombytes('RGBA',(width,height),data[16:]).save(destination)
                    return dict(width=width,height=height,scale=scale)
        time.sleep(.05)
    raise RuntimeError('No complete live frame available')


def run(args):
    out=Path(args.out);out.mkdir(parents=True,exist_ok=False)
    hvm=Path(args.hvm).resolve();ipa=Path(args.ipa).resolve();home=Path(args.home).resolve()
    plan=json.loads(Path(args.plan).read_text())
    inp=out/'input.txt';inp.touch();frame=out/'frame.rgba'
    command=[str(hvm),'run',str(ipa),'--home',str(home),'--run-for',str(args.seconds),'--graceful',
             '--report',str(out/'app.json'),'--screenshot',str(out/'final.png'),
             '--input-file',str(inp),'--frame-file',str(frame)]
    if args.gpu:command.append('--gpu')
    result=dict(schemaVersion=1,plan=plan.get('name'),steps=[],error=None,
                hostBinarySHA256=hashlib.sha256(hvm.read_bytes()).hexdigest(),
                guestIpaSHA256=hashlib.sha256(ipa.read_bytes()).hexdigest(),command=command,
                visualReviewRequired=True)
    began=time.monotonic()
    audio=CapturedAudio(out/'mixed.wav',began) if args.capture_audio else None
    if audio:command+=['--audio-fd',str(audio.writer.fileno())]
    with (out/'run.log').open('w') as log:
        try:
            proc=subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT,
                                  pass_fds=(audio.writer.fileno(),) if audio else ())
        except Exception:
            if audio:audio.close()
            raise
        def pause(seconds):
            until=time.monotonic()+seconds
            while time.monotonic()<until:
                if proc.poll() is not None:raise RuntimeError('App exited during UI plan')
                time.sleep(min(.05,max(0,until-time.monotonic())))
        def event(phase,x,y):
            with inp.open('a') as f:f.write(f'touch {phase} {x} {y}\n')
        try:
            # IPA import happens before the CLI's Process timer starts. A
            # cold import can exceed a fixed startup delay; do not inject
            # touches/stop into an app that has not produced its first frame.
            deadline=time.monotonic()+args.startup_timeout
            while not frame.exists():
                if time.monotonic()>=deadline:raise RuntimeError('Timed out waiting for the first live frame')
                pause(.05)
            result['startupFrame']=capture(frame,out/'startup.png')
            result['firstFrameSeconds']=round(time.monotonic()-began,3)
            pause(args.startup_seconds)
            for step in plan['steps']:
                item=dict(id=step['id'],expected=step['expected'],status='running',seconds=round(time.monotonic()-began,3))
                result['steps'].append(item)
                for action in step.get('actions',[]):
                    if 'tap' in action:
                        x,y=action['tap'];event(0,x,y);pause(.3);event(3,x,y);pause(.8)
                    elif 'wait' in action:pause(action['wait'])
                    elif 'drag' in action:
                        x,y,tx,ty=action['drag'];event(0,x,y);pause(.2)
                        for i in range(1,11):event(1,x+(tx-x)*i/10,y+(ty-y)*i/10);pause(.08)
                        event(3,tx,ty);pause(.8)
                    else:raise ValueError('Unsupported UI action')
                pause(step.get('settleSeconds',1.5))
                image=out/(step['id']+'.png');item['frame']=capture(frame,image)
                item.update(status='captured',screenshot=image.name)
                if audio:
                    end=time.monotonic()-began
                    item['audio']=audio.window(item['seconds'],end)
                    item['audioTail']=audio.window(max(item['seconds'],end-1),end)
                (out/'coverage.json').write_text(json.dumps(result,indent=2)+'\n')
        except Exception as error:
            result['error']=str(error)
            if result['steps'] and result['steps'][-1]['status']=='running':result['steps'][-1]['status']='interrupted'
        finally:
            if proc.poll() is None:
                with inp.open('a') as f:f.write('stop\n')
            try:result['exitCode']=proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                proc.kill();result['exitCode']=proc.wait();result['error']='Emulator did not stop gracefully'
            if audio:
                audio.close();result['audioCapture']=audio.window(0,time.monotonic()-began)
                if audio.error:result['error']=result['error'] or audio.error
    result['seconds']=round(time.monotonic()-began,2)
    if (out/'app.json').exists():
        report=json.loads((out/'app.json').read_text())
        result['app']={key:report.get(key) for key in ('crashed','exitStatus','loadError','perf','audio')}
        if report.get('crashed') or report.get('loadError'):result['error']=result['error'] or 'App report records a failure'
    (out/'coverage.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(dict(captured=sum(s['status']=='captured' for s in result['steps']),
                         planned=len(plan['steps']),error=result['error'],out=str(out),visualReviewRequired=True)))
    return 1 if result['error'] or result['exitCode'] else 0


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('hvm','ipa','home','plan','out'):p.add_argument('--'+name,required=True)
    p.add_argument('--gpu',action='store_true');p.add_argument('--seconds',type=int,default=300)
    p.add_argument('--capture-audio',action='store_true',help='Record mixed PCM and per-step signal windows (not a quality oracle)')
    p.add_argument('--startup-timeout',type=float,default=90,help='Maximum first-frame wait including IPA import; no automatic retry')
    p.add_argument('--startup-seconds',type=float,default=12,help='Additional settling time after the first live frame')
    raise SystemExit(run(p.parse_args()))
