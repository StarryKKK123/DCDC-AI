# -*- coding: utf-8 -*-
"""手工 cl2000 编译 300W 开环固件(含 int8 模型)"""
import io
import os
import subprocess
import sys

sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8')

PRJ = r'E:\TI\project\openloop_300W'
SDK = r'E:\MyTool\TI\core-sdk\c2000ware-core-sdk-main'
CGT = r'E:\MyTool\TI\CCS\ccs\tools\compiler\ti-cgt-c2000_25.11.1.LTS'
OUT = os.path.join(PRJ, 'build')
os.makedirs(OUT, exist_ok=True)

CFLAGS = ['-v28', '-ml', '-mt', '--float_support=fpu32', '--tmu_support=tmu0',
          '--define=_LAUNCHXL_F28P55X', '--define=DEBUG', '--define=_FLASH',
          '--define=__TI_EABI__', '--diag_warning=225', '--diag_suppress=10063',
          '--opt_level=2', '--c99',
          '-I' + os.path.join(CGT, 'include'),
          '-I' + PRJ, '-I' + os.path.join(PRJ, 'device'),
          '-I' + os.path.join(SDK, 'driverlib', 'f28p55x', 'driverlib'),
          '-I' + r'E:\MyTool\TI\py310\Lib\site-packages\tvm\standalone_crt\include',
          '-I' + r'E:\MyTool\TI\py310\Lib\site-packages\tvm\3rdparty\tinie-api',
          '-I' + r'E:\MyTool\TI\py310\Lib\site-packages\tvm\3rdparty\tinie-api\hard\inc']

srcs = [os.path.join(PRJ, 'app_openloop.c'),
        os.path.join(PRJ, 'device', 'device.c'),
        os.path.join(PRJ, 'ai_model', 'lib0.c'),
        os.path.join(PRJ, 'ai_model', 'lib1.c'),
        os.path.join(SDK, 'device_support', 'f28p55x', 'common', 'source', 'f28p55x_npu.c')]

objs = []
for c in srcs:
    o = os.path.join(OUT, os.path.basename(c).replace('.c', '.obj'))
    if os.path.exists(o):
        os.remove(o)
    r = subprocess.run([os.path.join(CGT, 'bin', 'cl2000.exe')] + CFLAGS +
                       ['-c', c, '--obj_directory=' + OUT],
                       capture_output=True, text=True, timeout=600)
    if r.returncode != 0 or not os.path.exists(o):
        print('编译失败:', os.path.basename(c))
        print((r.stdout + r.stderr)[-2500:])
        sys.exit(3)
    print('OK', os.path.basename(c))
    objs.append(o)

o = os.path.join(OUT, 'f28p55x_codestartbranch.obj')
if os.path.exists(o):
    os.remove(o)
subprocess.run([os.path.join(CGT, 'bin', 'cl2000.exe')] + CFLAGS +
               [os.path.join(PRJ, 'device', 'f28p55x_codestartbranch.asm'),
                '--obj_directory=' + OUT], capture_output=True, timeout=600)
objs.append(o)
print('OK codestart')

RTS = os.path.join(CGT, 'lib', 'rts2800_fpu32_eabi.lib')
DRV = os.path.join(SDK, 'driverlib', 'f28p55x', 'driverlib', 'ccs', 'Debug', 'driverlib.lib')
MOD = os.path.join(PRJ, 'ai_model', 'mod.a')
exe = os.path.join(OUT, 'openloop_int8.out')
r = subprocess.run([os.path.join(CGT, 'bin', 'cl2000.exe'), '--abi=eabi', '-v28', '-ml', '-mt',
                    '-z',
                    '--rom_model', '--define=__TI_EABI__',
                    '-i' + os.path.join(CGT, 'lib'),
                    '--stack_size=0x300', '--heap_size=0x300'] + objs +
                   [DRV, RTS, MOD, os.path.join(PRJ, 'lnk.cmd'),
                    '-o', exe, '--map_file=' + os.path.join(OUT, 'openloop_int8.map')],
                   capture_output=True, text=True, timeout=600)
if r.returncode != 0:
    print('链接失败:')
    print((r.stdout + r.stderr)[-3000:])
    sys.exit(4)
print('链接成功:', exe, '%.0f KB' % (os.path.getsize(exe) / 1024))

# CLEAN_OBJ_AFTER_LINK: 链接完成后删掉中间 .obj, 否则 CCS 会把工程里的 obj 当源文件重复链接
import glob as _glob
for _f in _glob.glob(os.path.join(OUT, '*.obj')):
    try:
        os.remove(_f)
    except OSError:
        pass
print('已清理中间 obj, 保留:', [os.path.basename(x) for x in _glob.glob(os.path.join(OUT, '*.out'))])
