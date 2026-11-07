from pathlib import Path
p=Path(r'D:\Local\HeliumOS-Beta\user\wm.c')
s=p.read_text(encoding='utf-8')
old='''            int occluded = 0;
            for (int j = 0; j < MAX_WINDOWS; j++) {'''
new='''            int occluded = (w->alpha > 0 && w->alpha < 255) ? 1 : 0;
            for (int j = 0; j < MAX_WINDOWS; j++) {'''
assert old in s
s=s.replace(old,new,1)
p.write_text(s,encoding='utf-8')
print('alpha dirty updates force composition')
