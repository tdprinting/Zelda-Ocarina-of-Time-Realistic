import os
import bpy,sys,math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); from render_lib import *
a=sys.argv[sys.argv.index('--')+1:]
bpy.ops.wm.open_mainfile(filepath=a[0])
co=setup_render((800,800),12)
i=2
while i+5<=len(a):
    tx,ty,tz,dist,az,el=map(float,a[i:i+6]); shoot(co,(tx,ty,tz),dist,az,el,f'{a[1]}_{i}.png'); i+=6
