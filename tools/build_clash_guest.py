#!/usr/bin/env python3
"""Build an experimental guest-local Clash IPA. Not the complete offline game.

Requires Linux clang++, clang and cctools ld64. Reads the privately supplied
pinned client. Generated client-derived fixtures and modified IPAs stay outside
the repository. No Frida, dynamic code generation or host Clash service.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import shutil
import struct
import subprocess
import zipfile
import copy
from package_clash_offline import SOURCE_SHA256, EXECUTABLE_SHA256
from clash_bot_policy import load_policy, export_header

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x800000


def macho(data):
    if len(data)<28 or struct.unpack_from('<II', data) != (0xfeedface,12):
        raise ValueError('Expected thin ARMv7 Mach-O')
    count, size = struct.unpack_from('<II', data, 16)
    result = dict(segments=[], sections=[], commands=[], symbols={})
    offset = 28
    symoff=None
    if 28+size>len(data):raise ValueError('Truncated load commands')
    for _ in range(count):
        command, length = struct.unpack_from('<II', data, offset)
        if length < 8 or offset + length > 28 + size:
            raise ValueError('Invalid load command')
        result['commands'].append((command, offset, length))
        if command == 1:
            name = data[offset+8:offset+24].split(b'\0')[0].decode()
            values = struct.unpack_from('<8I', data, offset+24)
            result['segments'].append((name, *values))
            for i in range(values[6]):
                at = offset+56+i*68
                result['sections'].append(struct.unpack_from('<16s16s9I', data, at))
        elif command == 2:
            symoff, nsyms, stroff, _ = struct.unpack_from('<4I', data, offset+8)
        elif command == 11:
            result['indirectoff'], _ = struct.unpack_from('<II', data, offset+56)
        elif command in (0x22, 0x80000022):
            fields = struct.unpack_from('<10I', data, offset+8)
            if any(fields[i] for i in (3, 5, 7)):
                result['hasBindings'] = True
        offset += length
    if offset!=28+size or symoff is None:
        raise ValueError('Missing symbol table or inconsistent load commands')
    names = []
    for i in range(nsyms):
        string, kind, section, desc, value = struct.unpack_from('<IBBHI', data, symoff+12*i)
        name = data[stroff+string:data.find(b'\0', stroff+string)].decode()
        names.append(name)
        if kind & 0xe == 0xe:
            result['symbols'][name] = value | (1 if desc & 8 else 0)
    result['stubs'] = {}
    for section in result['sections']:
        _, _, addr, size, _, _, _, _, flags, first, stride = section
        if flags & 255 == 8:
            for i in range(size//stride):
                index = struct.unpack_from('<I', data, result['indirectoff']+4*(first+i))[0]
                if index < len(names):
                    result['stubs'][names[index]] = addr+i*stride
    return result


def run(command, **kwargs):
    subprocess.run([str(v) for v in command], check=True, **kwargs)

def thumb_branch(source, target, link=False):
    """Four-byte Thumb B.W/BL; preserves adjacent instructions."""
    if source&1 or not target&1:raise ValueError('Expected Thumb branch addresses')
    offset=(target&~1)-(source+4)
    if offset&1 or not -(1<<24)<=offset<(1<<24):raise ValueError('Thumb branch out of range')
    value=offset&0x1ffffff;s=(value>>24)&1
    j1=1^((value>>23)&1)^s;j2=1^((value>>22)&1)^s
    return struct.pack('<HH',0xf000|(s<<10)|((value>>12)&1023),
                       (0xd000 if link else 0x9000)|(j1<<13)|(j2<<11)|((value>>1)&2047))

def build(source, output, build_dir, profile_saves=False, probe_donation_actors=False):
    source, output, build_dir = Path(source), Path(output), Path(build_dir)
    if output.exists() or build_dir.exists():
        raise ValueError('Choose new output and build paths; existing data will not be replaced')
    if hashlib.sha256(source.read_bytes()).hexdigest() != SOURCE_SHA256:
        raise ValueError('Pinned source IPA hash mismatch')
    build_dir.mkdir(parents=True)
    with zipfile.ZipFile(source) as archive:
        info_name = next(n for n in archive.namelist() if n.endswith('.app/Info.plist'))
        app_root = str(Path(info_name).parent)
        info = plistlib.loads(archive.read(info_name))
        exe_name = app_root+'/'+info['CFBundleExecutable']
        original = archive.read(exe_name)
        if hashlib.sha256(original).hexdigest() != EXECUTABLE_SHA256:
            raise ValueError('Pinned executable hash mismatch')
        # The whole input archive is pinned above. Reject traversal before extraction.
        for name in archive.namelist():
            if name.startswith('/') or '..' in Path(name).parts or '\\' in name:
                raise ValueError('Unsafe archive path')
        archive.extractall(build_dir/'source')
    parsed = macho(original)
    if max(vm+size for _, vm, size, *_ in parsed['segments']) > BASE:
        raise ValueError('Guest payload address overlaps original image')
    generator = build_dir/'fixture-export'
    bot_policy=load_policy(build_dir/'source'/app_root)
    policy_header=build_dir/'opponent-policy.h';policy_header.write_text(export_header(bot_policy))
    run(['clang++', '-std=c++20', '-O2', '-I'+str(ROOT/'src'),
         '-include',policy_header,
         ROOT/'tools/clash_guest/fixture_export.cpp', ROOT/'src/runtime/offline/ClashState.cpp',
         '-lz', '-o', generator])
    with (build_dir/'generated.h').open('w') as header:
        for symbol in ('puts', 'printf', 'malloc', 'abort', 'dlsym', 'sinf', 'cosf', 'memcpy', 'memset'):
            header.write(f'#define API_{symbol} 0x{parsed["stubs"]["_"+symbol]:x}u\n')
        run([generator, build_dir/'source'/app_root], stdout=header)
    run(['clang', '-target', 'armv7-apple-ios6.0', '-mthumb', '-Oz', '-ffreestanding',
         '-fno-builtin', '-fno-stack-protector', '-fno-pic', '-fno-unwind-tables',
         '-fno-asynchronous-unwind-tables', *(['-DGUEST_PROFILE_SAVES=1'] if profile_saves else []),
         *(['-DGUEST_PROBE_DONATION_ACTORS=1'] if probe_donation_actors else []), '-I'+str(build_dir), '-c',
         ROOT/'tools/clash_guest/transport.c', '-o', build_dir/'transport.o'])
    update_segment=next(s for s in parsed['segments'] if s[1]<=0x92f08<s[1]+s[4])
    update_at=update_segment[3]+0x92f08-update_segment[1]
    prologue=original[update_at:update_at+8]
    if prologue!=bytes.fromhex('f0b503af2de90005'):
        raise ValueError('Unexpected GameMode::update prologue; cannot relocate safely')
    # Preserve arguments, call the guest checkpoint, execute the original
    # PC-independent prologue, then continue the original function unchanged.
    assembly=build_dir/'update.s'
    assembly.write_text('.syntax unified\n.thumb\n.text\n.p2align 2\n.globl _guest_update\n'
        '.thumb_func\n_guest_update:\n push {r0-r4,lr}\n bl _guest_snapshot\n'
        ' pop.w {r0-r4,lr}\n .byte '+','.join(str(x) for x in prologue)+
        '\n .p2align 2\n .short 0xf8df,0xf000\n .long 0x92f11\n')
    reward_segment=next(s for s in parsed['segments'] if s[1]<=0x8203c<s[1]+s[4])
    reward_at=reward_segment[3]+0x8203c-reward_segment[1]
    reward_prologue=original[reward_at:reward_at+8]
    if reward_prologue!=bytes.fromhex('f0b503af2de9000d'):
        raise ValueError('Unexpected native reward prologue')
    with assembly.open('a') as asm:
        asm.write('.p2align 2\n.globl _guest_original_reward_execute\n.thumb_func\n'
                  '_guest_original_reward_execute:\n .byte '+','.join(str(x) for x in reward_prologue)+
                  '\n .p2align 2\n .short 0xf8df,0xf000\n .long 0x82045\n')
        settings_segment=next(s for s in parsed['segments'] if s[1]<=0x94f1c<s[1]+s[4])
        settings_at=settings_segment[3]+0x94f1c-settings_segment[1]
        settings_prologue=original[settings_at:settings_at+8]
        if settings_prologue!=bytes.fromhex('f0b503af2de9000d'):
            raise ValueError('Unexpected native settings-click prologue')
        asm.write('.p2align 2\n.globl _guest_original_settings_click\n.thumb_func\n'
                  '_guest_original_settings_click:\n .byte '+','.join(str(x) for x in settings_prologue)+
                  '\n .p2align 2\n .short 0xf8df,0xf000\n .long 0x94f25\n')
        battle_segment=next(s for s in parsed['segments'] if s[1]<=0x1499c<s[1]+s[4])
        battle_at=battle_segment[3]+0x1499c-battle_segment[1]
        battle_prologue=original[battle_at:battle_at+8]
        if battle_prologue!=bytes.fromhex('f0b503af2de9000d'):
            raise ValueError('Unexpected native battle-ended prologue')
        asm.write('.p2align 2\n.globl _guest_original_battle_ended\n.thumb_func\n'
                  '_guest_original_battle_ended:\n .byte '+','.join(str(x) for x in battle_prologue)+
                  '\n .p2align 2\n .short 0xf8df,0xf000\n .long 0x149a5\n')
        mail_segment=next(s for s in parsed['segments'] if s[1]<=0x37144<s[1]+s[4])
        mail_at=mail_segment[3]+0x37144-mail_segment[1]
        mail_prologue=original[mail_at:mail_at+8]
        if mail_prologue!=bytes.fromhex('f0b503af4df8048d'):
            raise ValueError('Unexpected native clan-mail prologue')
        asm.write('.p2align 2\n.globl _guest_original_mail_execute\n.thumb_func\n'
                  '_guest_original_mail_execute:\n .byte '+','.join(str(x) for x in mail_prologue)+
                  '\n .p2align 2\n .short 0xf8df,0xf000\n .long 0x3714d\n')
        clock_segment=next(s for s in parsed['segments'] if s[1]<=0xd6d40<s[1]+s[4])
        request_segment=next(s for s in parsed['segments'] if s[1]<=0x20654<s[1]+s[4])
        request_at=request_segment[3]+0x20654-request_segment[1]
        request_prologue=original[request_at:request_at+8]
        if request_prologue!=bytes.fromhex('f0b503af2de9000d'):
            raise ValueError('Unexpected native card-request prologue')
        asm.write('.p2align 2\n.globl _guest_original_request_execute\n.thumb_func\n'
                  '_guest_original_request_execute:\n .byte '+','.join(str(x) for x in request_prologue)+
                  '\n .p2align 2\n .short 0xf8df,0xf000\n .long 0x2065d\n')
        challenge_segment=next(s for s in parsed['segments'] if s[1]<=0x62ef8<s[1]+s[4])
        challenge_at=challenge_segment[3]+0x62ef8-challenge_segment[1]
        challenge_prologue=original[challenge_at:challenge_at+8]
        if challenge_prologue!=bytes.fromhex('f0b503af2de9000d'):
            raise ValueError('Unexpected native friendly-challenge prologue')
        asm.write('.p2align 2\n.globl _guest_original_challenge_execute\n.thumb_func\n'
                  '_guest_original_challenge_execute:\n .byte '+','.join(str(x) for x in challenge_prologue)+
                  '\n .p2align 2\n .short 0xf8df,0xf000\n .long 0x62f01\n')
        clock_at=clock_segment[3]+0xd6d40-clock_segment[1]
        clock_instructions=original[clock_at:clock_at+10]
        if clock_instructions!=bytes.fromhex('41f27a412046c0f23901'):
            raise ValueError('Unexpected native load-home clock instructions')
        # The first eight patched bytes overlap a four-byte MOVT. Relocate
        # all ten PC-independent bytes, then resume at the untouched ADD r1,pc
        # at its original address so its string reference remains correct.
        asm.write('.p2align 2\n.globl _guest_home_clock_hook\n.thumb_func\n'
                  '_guest_home_clock_hook:\n push {r0-r4,lr}\n mov r0,r4\n'
                  ' bl _guest_home_loaded\n pop.w {r0-r4,lr}\n .byte '+
                  ','.join(str(x) for x in clock_instructions)+
                  '\n .p2align 2\n .short 0xf8df,0xf000\n .long 0xd6d4b\n')
    run(['clang','-target','armv7-apple-ios6.0','-c',assembly,'-o',build_dir/'update.o'])
    linker = os.environ.get('HVM_LD64') or shutil.which('arm-apple-darwin11-ld')
    if not linker:
        for path in ('/home/lawson/cctools/bin/arm-apple-darwin11-ld', '/opt/cctools/bin/arm-apple-darwin11-ld'):
            if Path(path).exists(): linker = path; break
    if not linker:
        raise ValueError('Set HVM_LD64 to the cctools ld64 binary')
    payload_path = build_dir/'transport.dylib'
    run([linker, '-arch', 'armv7', '-static', '-e', '_guest_connect', '-iphoneos_version_min', '6.0',
         '-segaddr', '__TEXT', hex(BASE),
         '-no_uuid', '-o', payload_path, build_dir/'transport.o',build_dir/'update.o'])
    payload = payload_path.read_bytes()
    mod = macho(payload)
    if mod.get('hasBindings'):
        raise ValueError('Guest payload must not depend on new runtime imports')
    binary = bytearray(original)
    additions = bytearray()
    for name, vm, size, fileoff, filesize, maximum, initial, _, flags in mod['segments']:
        if name == '__LINKEDIT' or not size: continue
        destination = (len(binary)+4095)&~4095
        binary.extend(bytes(destination-len(binary)))
        binary.extend(payload[fileoff:fileoff+filesize])
        unique = ('__GO_'+name.strip('_')).encode().ljust(16,b'\0')
        additions.extend(struct.pack('<II16s8I', 1, 56, unique, vm, size,
                                     destination, filesize, maximum, initial, 0, flags))
    count, size = struct.unpack_from('<II', binary,16)
    end = 28+size
    first = min(s[4] for s in parsed['sections'] if s[4])
    if end+len(additions)>first or any(binary[end:end+len(additions)]):
        raise ValueError('Insufficient zero header padding for guest segments')
    binary[end:end+len(additions)] = additions
    struct.pack_into('<II', binary,16,count+len(additions)//56,size+len(additions))
    # Absolute pinned-client calls and segment addresses deliberately use no ASLR.
    flags = struct.unpack_from('<I', binary,24)[0]
    struct.pack_into('<I', binary,24,flags & ~0x200000)
    patches=[]
    for address,expected in [(0x100bfc,'44f28e01c0f23601'),(0x100c10,'44f27e01c0f23601'),
                             (0x100c24,'44f26e01c0f23601'),(0x100c38,'006d704780b56f46')]:
        segment=next(s for s in parsed['segments'] if s[1]<=address<s[1]+s[4])
        at=segment[3]+address-segment[1]
        if original[at:at+8]!=bytes.fromhex(expected):raise ValueError('Unexpected native AI getter')
    for address, symbol in [(0x187484,'_guest_connect'),(0x1872d0,'_guest_send'),
                            (0x187130,'_guest_disconnect'),(0x92f08,'_guest_update'),
                            (0x9ae94,'_guest_set_npc_wins'),(0x9aea4,'_guest_set_npc_losses'),
                            (0x8203c,'_guest_reward_execute'),(0x94f1c,'_guest_settings_click'),
                            (0x1499c,'_guest_battle_ended'),(0xd6d40,'_guest_home_clock_hook'),
                            (0x37144,'_guest_mail_execute'),(0x20654,'_guest_request_execute'),
                            (0x62ef8,'_guest_challenge_execute'),
                            (0x100bfc,'_guest_npc_mana'),(0x100c10,'_guest_npc_mana_end'),
                            (0x100c24,'_guest_npc_mana_overtime')]:
        segment = next(s for s in parsed['segments'] if s[1]<=address<s[1]+s[4])
        at = segment[3]+address-segment[1]
        target = mod['symbols'][symbol]
        if address%4: raise ValueError('Branch literal requires aligned patch address')
        binary[at:at+8] = bytes.fromhex('dff800f0')+struct.pack('<I',target)
        patches.append(dict(address=hex(address),function=symbol,target=hex(target)))
    for address,symbol,expected in [(0x100c38,'_guest_npc_reserve','006d7047'),
                                    (0x99e48,'_guest_avatar_set_name','2030f4f0')]:
        segment=next(s for s in parsed['segments'] if s[1]<=address<s[1]+s[4])
        at=segment[3]+address-segment[1];target=mod['symbols'][symbol]
        if original[at:at+4]!=bytes.fromhex(expected):raise ValueError('Unexpected native short getter/setter')
        binary[at:at+4]=thumb_branch(address,target)
        patches.append(dict(address=hex(address),function=symbol,target=hex(target),width=4))
    address=0xc867c;segment=next(s for s in parsed['segments'] if s[1]<=address<s[1]+s[4])
    at=segment[3]+address-segment[1];target=mod['symbols']['_guest_npc_label']
    if original[at:at+4]!=thumb_branch(address,0x511a9,link=True):raise ValueError('Unexpected native NPC label call')
    binary[at:at+4]=thumb_branch(address,target,link=True)
    patches.append(dict(address=hex(address),function='_guest_npc_label',target=hex(target),width=4,link=True))
    for address,width in [(0x100c3c,4),(0x99e44,4),(0x99e4c,4),(0xc8680,4)]:
        segment=next(s for s in parsed['segments'] if s[1]<=address<s[1]+s[4])
        at=segment[3]+address-segment[1]
        if binary[at:at+width]!=original[at:at+width]:raise ValueError('Adjacent native code changed')
    # This ARM import stub is executable code; the original lazy-binding data
    # remains untouched. LDR pc,[pc,#-4] interworks to the Thumb guest shim.
    trig_stub=parsed['stubs']['___sincosf_stret']
    segment=next(s for s in parsed['segments'] if s[1]<=trig_stub<s[1]+s[4])
    at=segment[3]+trig_stub-segment[1];target=mod['symbols']['_guest_sincosf']
    binary[at:at+8]=struct.pack('<II',0xe51ff004,target)
    patches.append(dict(address=hex(trig_stub),function='_guest_sincosf',target=hex(target),mode='ARM import stub'))
    info['CFBundleIdentifier']='com.hydrogenvm.clashroyale.guestdev'
    info['CFBundleDisplayName']='Clash Offline Guest DEV'
    info.pop('HydrogenVMOfflineBackend',None)
    info['ClashGuestOfflinePrototypeVersion']=1
    output.parent.mkdir(parents=True,exist_ok=True)
    with zipfile.ZipFile(source) as archive, output.open('xb') as dest:
        with zipfile.ZipFile(dest,'w') as packaged:
            for member in archive.infolist():
                content = binary if member.filename==exe_name else (
                    plistlib.dumps(info,fmt=plistlib.FMT_BINARY) if member.filename==info_name else archive.read(member.filename))
                packaged.writestr(copy.copy(member),content)
    with zipfile.ZipFile(source) as before, zipfile.ZipFile(output) as after:
        if before.namelist()!=after.namelist() or after.testzip(): raise ValueError('Archive verification failed')
        for name in before.namelist():
            if name not in (exe_name,info_name) and before.read(name)!=after.read(name):
                raise ValueError('Unexpected asset change: '+name)
    report=dict(prototype=True,sha256=hashlib.sha256(output.read_bytes()).hexdigest(),
                originalExecutableSHA256=EXECUTABLE_SHA256,
                modifiedExecutableSHA256=hashlib.sha256(binary).hexdigest(),patches=patches,
                hostClashBackendRequired=False,networkRequired=False,saveProfiling=profile_saves,
                donationActorProbe=probe_donation_actors,
                bundleIdentifier=info['CFBundleIdentifier'],assetsUnchanged=True,
                trainingMissionsEncoded=7,
                arenaLevelsEncoded=[1],arenaBotsPerLevel=18,
                opponentDecks=[d['name'] for d in bot_policy['decks']],
                limitations=['experimental offline prototype; full earned progression unqualified',
                             'later arenas, draw accounting and remaining economy paths incomplete',
                             'no device qualification','not a native modern-iOS app'])
    output.with_suffix('.manifest.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report,indent=2))


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source_ipa',type=Path)
    parser.add_argument('output_ipa',type=Path)
    parser.add_argument('--build-dir',required=True,type=Path)
    parser.add_argument('--profile-saves',action='store_true',help='Log slow native checkpoint timings without changing persistence behavior')
    parser.add_argument('--probe-donation-actors',action='store_true',
                        help='Diagnostic candidate: verify native transfers on independent actors, without granting live-player cards')
    args=parser.parse_args()
    build(args.source_ipa,args.output_ipa,args.build_dir,args.profile_saves,args.probe_donation_actors)
